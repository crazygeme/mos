#include <ps/ps.h>
#include <device/time.h>
#include <mm/mmap.h>
#include <errno.h>
#include <macro.h>
#include <lib/klib.h>

#include <syscall/syscall.h>
#include "ps_internal.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAITERS 0x80000000U
#define FUTEX_OWNER_DIED 0x40000000U
#define FUTEX_TID_MASK 0x3fffffffU
#define ROBUST_LIST_LIMIT 2048

static int robust_read(task_struct *task, uintptr_t addr, void *dst,
		       unsigned len)
{
	return ps_read_process_memory(task, (void *)(uintptr_t)addr, dst, len);
}

static void robust_release_futex(task_struct *task, uintptr_t entry,
				 intptr_t offset)
{
	uintptr_t addr;
	unsigned value, owner_died;
	int irq;

	if (!entry || (uintptr_t)(entry + offset) < PAGE_SIZE)
		return;
	addr = entry + offset;
	if (robust_read(task, addr, &value, sizeof(value)) != 0 ||
	    (value & FUTEX_TID_MASK) != task->psid)
		return;
	owner_died = (value & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
	if (ps_write_process_memory(task, (void *)(uintptr_t)addr, &owner_died,
				    sizeof(owner_died)) != 0)
		return;
	if (value & FUTEX_WAITERS) {
		spinlock_lock(&ps_lock, &irq);
		ps_futex_wake_locked(task->user, (int *)(uintptr_t)addr, 1);
		spinlock_unlock(&ps_lock, irq);
	}
}

void ps_release_robust_list(task_struct *task)
{
	uintptr_t base = (uintptr_t)task->robust_list_head, entry, next,
		  pending;
	intptr_t offset;
	if (!base || !task->robust_list_reader)
		return;
	if (task->robust_list_reader(task, base, &entry, &offset, &pending))
		return;
	entry &= ~(uintptr_t)1;
	pending &= ~(uintptr_t)1;
	for (unsigned count = 0;
	     entry && entry != base && count < ROBUST_LIST_LIMIT; count++) {
		next = 0;
		if (task->robust_list_reader(task, entry, &next, NULL, NULL))
			break;
		if (entry != pending)
			robust_release_futex(task, entry, offset);
		entry = next & ~(uintptr_t)1;
	}
	if (pending)
		robust_release_futex(task, pending, offset);
	task->robust_list_head = NULL;
}

typedef struct futex_key {
	unsigned kind;
	void *identity;
	uint64_t object;
	uintptr_t offset;
	file *backing;
} futex_key;

/* Shared mappings use backing-object offsets, independent of virtual address. */
static int futex_get_key(user_enviroment *user, int *uaddr, int private,
			 futex_key *key)
{
	vm_struct_t vm = user->vm;
	struct rb_node *node;
	vm_region *region = NULL;
	uintptr_t addr = (uintptr_t)uaddr;
	int irq;

	memset(key, 0, sizeof(*key));
	key->identity = vm;
	key->offset = addr;
	if (private)
		return 0;
	if (!vm)
		return -EFAULT;
	spinlock_lock(&vm->vma_lock, &irq);
	node = vm->vma_index.rb_node;
	while (node) {
		vm_region *r = rb_entry(node, vm_region, rb_node);
		if (addr < r->begin)
			node = node->rb_left;
		else if (addr >= r->end)
			node = node->rb_right;
		else {
			region = r;
			break;
		}
	}
	if (!region || region->end - addr < sizeof(int) ||
	    !(region->prot & PROT_READ)) {
		spinlock_unlock(&vm->vma_lock, irq);
		return -EFAULT;
	}
	if (region->flag & MAP_SHARED) {
		if (region->fp && region->fp->f_inode) {
			inode *in = region->fp->f_inode;
			key->kind = 1;
			key->identity = in->i_pgcache_tag ? in->i_pgcache_tag :
							    in->i_private;
			key->object = in->i_ino;
			key->backing = region->fp;
			fs_get_file(key->backing);
		} else if (region->anon_id) {
			key->kind = 2;
			key->identity = NULL;
			key->object = region->anon_id;
		}
		if (key->kind)
			key->offset =
				(unsigned)region->offset + addr - region->begin;
	}
	spinlock_unlock(&vm->vma_lock, irq);
	return 0;
}

static int futex_keys_equal(const futex_key *a, const futex_key *b)
{
	return a->kind == b->kind && a->identity == b->identity &&
	       a->object == b->object && a->offset == b->offset;
}

static void futex_release_key(void *arg)
{
	futex_key *key = arg;
	if (key->backing) {
		file *fp = key->backing;
		key->backing = NULL;
		fs_put_file(fp);
	}
}

typedef struct futex_waiter {
	task_struct *task;
	futex_key key;
	int woken;
	unsigned bitset;
	list_entry list;
} futex_waiter;

static list_entry futex_waiters;

struct futex_timespec64 {
	int64_t tv_sec;
	int64_t tv_nsec;
};

static void futex_init(void)
{
	list_init(&futex_waiters);
}

KERNEL_INIT(7, futex_init);

static int futex_wake_mask_locked(const futex_key *key, int max_wake,
				  unsigned bitset)
{
	list_entry *entry = futex_waiters.next;
	int n = 0;

	while (entry != &futex_waiters && n < max_wake) {
		futex_waiter *w = container_of(entry, futex_waiter, list);
		entry = entry->next;
		if (!futex_keys_equal(&w->key, key) || !(w->bitset & bitset))
			continue;
		w->woken = 1;
		list_remove_entry(&w->list);
		ps_put_to_ready_queue_unsafe(w->task);
		n++;
	}

	return n;
}

int ps_futex_wake_locked(user_enviroment *user, int *uaddr, int max_wake)
{
	futex_key key = { 0 };
	key.identity = user->vm;
	key.offset = (uintptr_t)uaddr;
	return futex_wake_mask_locked(&key, max_wake, ~0U);
}

void ps_futex_remove_task_locked(task_struct *task)
{
	list_entry *entry = futex_waiters.next;

	while (entry != &futex_waiters) {
		futex_waiter *w = container_of(entry, futex_waiter, list);
		entry = entry->next;
		if (w->task == task)
			list_remove_entry(&w->list);
	}
}

void ps_clear_child_tid(task_struct *task)
{
	int irq;
	vm_region *region;

	if (!task->clear_child_tid)
		return;

	region = task->user->vm ?
			 vm_find_map_cached(task->user,
					    (uintptr_t)task->clear_child_tid) :
			 NULL;
	if (region && region->begin <= (uintptr_t)task->clear_child_tid &&
	    (uintptr_t)task->clear_child_tid + sizeof(int) <= region->end &&
	    (region->prot & PROT_WRITE)) {
		int zero = 0;
		if (ps_write_process_memory(task, task->clear_child_tid, &zero,
					    sizeof(zero)) < 0) {
			task->clear_child_tid = NULL;
			return;
		}
		spinlock_lock(&ps_lock, &irq);
		ps_futex_wake_locked(task->user, task->clear_child_tid, 1);
		spinlock_unlock(&ps_lock, irq);
	}

	task->clear_child_tid = NULL;
}

static int futex_execute(int *uaddr, int op, int val, const void *timeout,
			 int *uaddr2, int val3, int time64,
			 const futex_key *key)
{
	task_struct *cur = CURRENT_TASK();
	futex_waiter waiter;
	unsigned timeout_ms;
	uint64_t deadline_ms = 0;
	int irq;
	int n = 0;
	int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
	struct futex_timespec64 ts;
	unsigned bitset = ~0U;
	int value;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("futex(%x, %d, %d, %x, %x, %d)\n", uaddr, op, val, timeout,
		     uaddr2, val3);

	(void)uaddr2;
	(void)val3;

	if (!uaddr)
		return -EFAULT;
	if ((unsigned)(uintptr_t)uaddr & 3)
		return -EINVAL;
	if ((op & FUTEX_CLOCK_REALTIME) && cmd != FUTEX_WAIT_BITSET)
		return -ENOSYS;
	if (cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_WAKE_BITSET) {
		bitset = (unsigned)val3;
		if (!bitset)
			return -EINVAL;
	}

	switch (cmd) {
	case FUTEX_WAIT:
	case FUTEX_WAIT_BITSET:
		if (ps_read_process_memory(cur, uaddr, &value, sizeof(value)) <
		    0)
			return -EFAULT;
		if (value != val)
			return -EAGAIN;

		timeout_ms = 0;
		if (timeout) {
			uint64_t ms;
			if (time64) {
				if (ps_read_process_memory(cur, timeout, &ts,
							   sizeof(ts)) < 0)
					return -EFAULT;
			} else {
				struct timespec old;
				if (ps_read_process_memory(cur, timeout, &old,
							   sizeof(old)) < 0)
					return -EFAULT;
				ts.tv_sec = old.tv_sec;
				ts.tv_nsec = old.tv_nsec;
			}
			if (ts.tv_sec < 0 || ts.tv_nsec < 0 ||
			    ts.tv_nsec >= 1000000000)
				return -EINVAL;
			if (cmd == FUTEX_WAIT_BITSET) {
				uint64_t now = (op & FUTEX_CLOCK_REALTIME) ?
						       time_wall_us() :
						       time_now_us();
				ts.tv_sec -= (int64_t)(now / 1000000ULL);
				ts.tv_nsec -=
					(int64_t)((now % 1000000ULL) * 1000);
				if (ts.tv_nsec < 0) {
					ts.tv_sec--;
					ts.tv_nsec += 1000000000;
				}
			}
			if (ts.tv_sec < 0 || (!ts.tv_sec && !ts.tv_nsec))
				return -ETIMEDOUT;
			if ((uint64_t)ts.tv_sec > (~0ULL - 1000) / 1000)
				ms = ~0ULL;
			else
				ms = (uint64_t)ts.tv_sec * 1000 +
				     (ts.tv_nsec + 999999) / 1000000;
			deadline_ms = time_now_ms();
			deadline_ms = ms > ~0ULL - deadline_ms ?
					      ~0ULL :
					      deadline_ms + ms;
		}
wait_again:
		if (timeout) {
			uint64_t now = time_now_ms();
			uint64_t remaining;
			if (now >= deadline_ms)
				return -ETIMEDOUT;
			remaining = deadline_ms - now;
			timeout_ms = remaining > 0xffffffffULL ?
					     0xffffffffU :
					     (unsigned)remaining;
		}
		memset(&waiter, 0, sizeof(waiter));
		waiter.task = cur;
		waiter.key = *key;
		waiter.bitset = bitset;
		list_init(&waiter.list);

		spinlock_lock(&ps_lock, &irq);
		if (*uaddr != val) {
			spinlock_unlock(&ps_lock, irq);
			return -EAGAIN;
		}
		if (ps_interrupting_signals(cur)) {
			spinlock_unlock(&ps_lock, irq);
			return -EINTR;
		}
		list_insert_tail(&futex_waiters, &waiter.list);
		if (timeout_ms > 0)
			timer_arm_unsafe(cur, timeout_ms);
		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
		cur->wait_interruptible = 1;
		spinlock_unlock(&ps_lock, irq);

		task_sched();

		spinlock_lock(&ps_lock, &irq);
		if (!waiter.woken)
			list_remove_entry(&waiter.list);
		timer_disarm_unsafe(cur);
		n = waiter.woken;
		spinlock_unlock(&ps_lock, irq);

		if (n)
			return 0;
		if (ps_interrupting_signals(cur))
			return -EINTR;
		if (!timeout || time_now_ms() < deadline_ms)
			goto wait_again;
		return -ETIMEDOUT;

	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		if (val < 0)
			return -EINVAL;

		spinlock_lock(&ps_lock, &irq);
		n = futex_wake_mask_locked(key, val, bitset);
		spinlock_unlock(&ps_lock, irq);
		return n;

	default:
		return -ENOSYS;
	}
}

static int futex_common(int *uaddr, int op, int val, const void *timeout,
			int *uaddr2, int val3, int time64)
{
	task_struct *cur = CURRENT_TASK();
	futex_key key;
	int result;
	int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

	if (!uaddr)
		return -EFAULT;
	if ((unsigned)(uintptr_t)uaddr & 3)
		return -EINVAL;
	if (cmd != FUTEX_WAIT && cmd != FUTEX_WAIT_BITSET &&
	    cmd != FUTEX_WAKE && cmd != FUTEX_WAKE_BITSET)
		return -ENOSYS;
	result = futex_get_key(cur->user, uaddr, op & FUTEX_PRIVATE_FLAG, &key);
	if (result < 0)
		return result;
	cur->cancel_io_wait = futex_release_key;
	cur->io_wait = &key;
	result = futex_execute(uaddr, op, val, timeout, uaddr2, val3, time64,
			       &key);
	cur->cancel_io_wait = NULL;
	cur->io_wait = NULL;
	futex_release_key(&key);
	return result;
}

int sys_futex(int *uaddr, int op, int val, const struct timespec *timeout,
	      int *uaddr2, int val3)
{
	return futex_common(uaddr, op, val, timeout, uaddr2, val3, 0);
}

int sys_futex_time64(int *uaddr, int op, int val, const void *timeout,
		     int *uaddr2, int val3)
{
	return futex_common(uaddr, op, val, timeout, uaddr2, val3, 1);
}
