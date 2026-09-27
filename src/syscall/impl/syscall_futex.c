#include <ps/ps.h>
#include <hw/time.h>
#include <mm/mmap.h>
#include <errno.h>
#include <macro.h>
#include <lib/klib.h>

#include <ps/impl/ps_internal.h>

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

struct robust_list_head_compat {
	unsigned list_next;
	int futex_offset;
	unsigned list_op_pending;
};

static int robust_read(task_struct *task, unsigned addr, void *dst,
		       unsigned len)
{
	return ps_read_process_memory(task, (void *)addr, dst, len);
}

static void robust_release_futex(task_struct *task, unsigned entry, int offset)
{
	unsigned addr;
	unsigned value, owner_died;
	int irq;

	if (!entry || (unsigned)(entry + offset) < PAGE_SIZE)
		return;
	addr = entry + offset;
	if (robust_read(task, addr, &value, sizeof(value)) != 0 ||
	    (value & FUTEX_TID_MASK) != task->psid)
		return;
	owner_died = (value & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
	if (ps_write_process_memory(task, (void *)addr, &owner_died,
				    sizeof(owner_died)) != 0)
		return;
	if (value & FUTEX_WAITERS) {
		spinlock_lock(&ps_lock, &irq);
		ps_futex_wake_locked(task->user, (int *)addr, 1);
		spinlock_unlock(&ps_lock, irq);
	}
}

void ps_release_robust_list(task_struct *task)
{
	struct robust_list_head_compat head;
	unsigned base = (unsigned)task->robust_list_head;
	unsigned entry, next, pending;
	unsigned count;

	if (!base || robust_read(task, base, &head, sizeof(head)) != 0)
		return;
	entry = head.list_next & ~1U;
	pending = head.list_op_pending & ~1U;
	for (count = 0; entry && entry != base && count < ROBUST_LIST_LIMIT;
	     count++) {
		if (robust_read(task, entry, &next, sizeof(next)) != 0)
			break;
		if (entry != pending)
			robust_release_futex(task, entry, head.futex_offset);
		entry = next & ~1U;
	}
	if (pending)
		robust_release_futex(task, pending, head.futex_offset);
	task->robust_list_head = NULL;
}

typedef struct futex_waiter {
	task_struct *task;
	user_enviroment *user;
	int *uaddr;
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

static int futex_wake_mask_locked(user_enviroment *user, int *uaddr,
				  int max_wake, unsigned bitset)
{
	list_entry *entry = futex_waiters.next;
	int n = 0;

	while (entry != &futex_waiters && n < max_wake) {
		futex_waiter *w = container_of(entry, futex_waiter, list);
		entry = entry->next;
		if (w->user->vm != user->vm || w->uaddr != uaddr ||
		    !(w->bitset & bitset))
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
	return futex_wake_mask_locked(user, uaddr, max_wake, ~0U);
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
					    (unsigned)task->clear_child_tid) :
			 NULL;
	if (region && region->begin <= (unsigned)task->clear_child_tid &&
	    (unsigned)task->clear_child_tid + sizeof(int) <= region->end &&
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

static int futex_common(int *uaddr, int op, int val, const void *timeout,
			int *uaddr2, int val3, int time64)
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
	if ((unsigned)uaddr & 3)
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
		waiter.user = cur->user;
		waiter.uaddr = uaddr;
		waiter.bitset = bitset;
		list_init(&waiter.list);

		spinlock_lock(&ps_lock, &irq);
		if (*uaddr != val) {
			spinlock_unlock(&ps_lock, irq);
			return -EAGAIN;
		}
		list_insert_tail(&futex_waiters, &waiter.list);
		if (timeout_ms > 0)
			timer_arm_unsafe(cur, timeout_ms);
		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
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
		if (cur->signal->sig_pending & ~cur->signal->sig_mask)
			return -EINTR;
		if (!timeout || time_now_ms() < deadline_ms)
			goto wait_again;
		return -ETIMEDOUT;

	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		if (val < 0)
			return -EINVAL;

		spinlock_lock(&ps_lock, &irq);
		n = futex_wake_mask_locked(cur->user, uaddr, val, bitset);
		spinlock_unlock(&ps_lock, irq);
		return n;

	default:
		return -ENOSYS;
	}
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
