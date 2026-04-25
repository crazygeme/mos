#include <ps/ps.h>
#include <mm/mmap.h>
#include <errno.h>
#include <macro.h>
#include <lib/klib.h>

#include "../ps/ps_internal.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

typedef struct futex_waiter {
	task_struct *task;
	user_enviroment *user;
	int *uaddr;
	int woken;
	list_entry list;
} futex_waiter;

static list_entry futex_waiters;

static unsigned futex_timeout_ms(const struct timespec *timeout)
{
	unsigned long long ms;

	if (!timeout)
		return 0;

	ms = (unsigned long long)timeout->tv_sec * 1000ULL;
	ms += (unsigned long long)timeout->tv_nsec / 1000000ULL;
	if (ms > 0xffffffffULL)
		ms = 0xffffffffULL;
	return (unsigned)ms;
}

static void futex_init(void)
{
	list_init(&futex_waiters);
}

KERNEL_INIT(7, futex_init);

int ps_futex_wake_locked(user_enviroment *user, int *uaddr, int max_wake)
{
	list_entry *entry = futex_waiters.next;
	int n = 0;

	while (entry != &futex_waiters && n < max_wake) {
		futex_waiter *w = container_of(entry, futex_waiter, list);
		entry = entry->next;
		if (w->user->vm != user->vm || w->uaddr != uaddr)
			continue;
		w->woken = 1;
		list_remove_entry(&w->list);
		ps_put_to_ready_queue_unsafe(w->task);
		n++;
	}

	return n;
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
		spinlock_lock(&ps_lock, &irq);
		*task->clear_child_tid = 0;
		ps_futex_wake_locked(task->user, task->clear_child_tid, 1);
		spinlock_unlock(&ps_lock, irq);
	}

	task->clear_child_tid = NULL;
}

int sys_futex(int *uaddr, int op, int val, const struct timespec *timeout,
	      int *uaddr2, int val3)
{
	task_struct *cur = CURRENT_TASK();
	futex_waiter waiter;
	unsigned timeout_ms;
	int irq;
	int n = 0;
	int cmd = op & ~FUTEX_PRIVATE_FLAG;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("futex(%x, %d, %d, %x, %x, %d)\n", uaddr, op, val, timeout,
		     uaddr2, val3);

	(void)uaddr2;
	(void)val3;

	if (!uaddr)
		return -EFAULT;

	switch (cmd) {
	case FUTEX_WAIT:
		if (*uaddr != val)
			return -EAGAIN;

		timeout_ms = futex_timeout_ms(timeout);
		memset(&waiter, 0, sizeof(waiter));
		waiter.task = cur;
		waiter.user = cur->user;
		waiter.uaddr = uaddr;
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
		return -ETIMEDOUT;

	case FUTEX_WAKE:
		if (val < 0)
			return -EINVAL;

		spinlock_lock(&ps_lock, &irq);
		n = ps_futex_wake_locked(cur->user, uaddr, val);
		spinlock_unlock(&ps_lock, irq);
		return n;

	default:
		return -ENOSYS;
	}
}
