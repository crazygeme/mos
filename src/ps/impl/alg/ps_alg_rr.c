/*
 * ps_sched.c — Multilevel Priority Ready Queue scheduler and context switch.
 *
 * Owns:
 *   - Ready/wait/dying queue transitions
 *   - MPRQ pick algorithm (RB-tree per priority level)
 *   - Context switch (_task_sched)
 *   - Scheduling instrumentation
 */

#include "fs/fs.h"
#include <ps/ps.h>
#include <ps/signal.h>
#include <lib/klib.h>
#include <mm/mm.h>
#include <int/int.h>
#include <int/dsr.h>
#include <lib/list.h>
#include <macro.h>
#include <config.h>
#include <errno.h>
#include <ps/smp.h>

extern unsigned long long gdt[];

#include "../ps_internal.h"

/*
 * Static globals
 */

/* Monotonically increasing tick; lower sched_seq = older = runs first. */

/*
 * Static helpers — MPRQ algorithm
 */

/* Return the first runnable task from the given RB-tree.
 * Skips sleeping (timeout in the future) and dying tasks.
 * Re-inserts the chosen task with a fresh sched_seq for round-robin fairness.
 * Must be called with ps_lock held. */
static task_struct *ps_get_available_ready_task(list_entry *head)
{
	list_entry *node = head->next;

	while (node != head) {
		task_struct *task =
			(container_of(node, task_schedule, ps_list)->task);
		if ((task->sched->status == ps_ready || task == current) &&
		    (!task->sched->terminate_requested ||
		     task->sched->vm_lock_depth) &&
		    (!task->sched->on_cpu || task == current) &&
		    (task->sched->priority != ps_idle ||
		     task->life->param == (void *)(uintptr_t)smp_cpu_id())) {
			list_remove_entry(node);
			list_insert_tail(head, &task->sched->ps_list);
			return task;
		}
		node = node->next;
	}
	return NULL;
}

/*
 * Timer queue helpers — all called with ps_lock held.
 *
 * timer_arm_unsafe: insert @task into the global timer RB-tree with
 *   expiry = now + ms.  Duplicate due times go to the right so that
 *   rb_first() always returns the earliest entry.
 *
 * timer_disarm_unsafe: remove @task from the timer tree if it is present.
 *
 * ps_fire_timers_unsafe: move every expired task (due_ms <= now) to the
 *   ready queue.  Called once per scheduling decision to avoid idle spinning.
 */
void timer_arm_unsafe(task_struct *task, unsigned ms)
{
	struct rb_root *root = &control.timer_queue;
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;
	unsigned long long due = time_deadline_ms(ms);

	task->wait->timer_due_ms = due;
	while (*link) {
		task_struct *t = (rb_entry(*link, task_wait, timer_rb)->task);
		parent = *link;
		if (due < t->wait->timer_due_ms)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&task->wait->timer_rb, parent, link);
	rb_insert_color(&task->wait->timer_rb, root);
}

void timer_disarm_unsafe(task_struct *task)
{
	if (!RB_EMPTY_NODE(&task->wait->timer_rb)) {
		rb_erase(&task->wait->timer_rb, &control.timer_queue);
		RB_CLEAR_NODE(&task->wait->timer_rb);
		task->wait->timer_due_ms = 0;
	}
}

void ps_fire_timers_unsafe(void)
{
	unsigned long long now = time_coarse_ms();
	struct rb_node *n = rb_first(&control.timer_queue);

	while (n) {
		task_struct *t = (rb_entry(n, task_wait, timer_rb)->task);
		if (t->wait->timer_due_ms > now)
			break;
		n = rb_next(n);
		rb_erase(&t->wait->timer_rb, &control.timer_queue);
		RB_CLEAR_NODE(&t->wait->timer_rb);
		t->wait->timer_due_ms = 0;
		ps_put_to_ready_queue_unsafe(t);
	}
}

/* Scan ready levels from highest to lowest and return the next task to run. */
task_struct *ps_get_next_task_unsafe(void)
{
	task_struct *task = NULL;
	int i = PS_PRIORITY_MAX - 1;

	if (current->life->psid != 0xffffffff)
		ps_fire_timers_unsafe();
	for (; i >= 0; i--) {
		if (list_is_empty(&control.ready_queue[i]))
			continue;
		task = ps_get_available_ready_task(&control.ready_queue[i]);
		if (task)
			break;
	}
	return task;
}

/*
 * Public — queue transitions
 */

/* Move task to the dying queue and notify its parent.
 * Status is set to ps_dying only after enqueueing so a preemption between the
 * two steps cannot lose the task (the scheduler skips ps_dying tasks). */
void ps_put_to_dying_queue_unsafe(task_struct *task)
{
	task_struct *parent = ps_find_process_unsafe(task->life->ppid);
	task_struct *init_process = ps_find_process_unsafe(1);
	list_entry *entry;
	int moved_children = 0;

	while (!list_is_empty(&task->life->dying_queue)) {
		task_struct *child;

		entry = list_remove_head(&task->life->dying_queue);
		child = (container_of(entry, task_schedule, ps_list)->task);
		if (init_process) {
			child->life->ppid = init_process->life->psid;
			list_insert_tail(&init_process->life->dying_queue,
					 entry);
			moved_children = 1;
		}
	}
	if (moved_children && init_process && init_process->sighand) {
		ps_queue_group_signal_unsafe(init_process, SIGCHLD);
		if (init_process->sched->status == ps_waiting)
			ps_put_to_ready_queue_unsafe(init_process);
	}

	if (!parent || parent->sched->status == ps_dying) {
		parent = init_process;
		if (parent)
			task->life->ppid = parent->life->psid;
	}

	if (!parent || parent->sched->status == ps_dying)
		klog("Can't find parent process %d or 1\n", task->life->ppid);

	list_remove_entry(&task->sched->ps_list);

	if (task->life->psid != 0xffffffff && parent)
		list_insert_tail(&parent->life->dying_queue,
				 &task->sched->ps_list);

	ps_alarm_disarm_unsafe(task);
	task->sched->status = ps_dying;
	task->wait->wait_func = NULL;
}

void ps_put_to_dying_queue(task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	ps_put_to_dying_queue_unsafe(task);
	if (task->life->ppid && task->life->exit_signal > 0 &&
	    task->life->exit_signal < NSIG) {
		task_struct *parent = ps_find_process_unsafe(task->life->ppid);

		if (!parent)
			goto out;
		/* Queue the requested exit signal before waking the parent so the
		 * already pending when wait() returns to userspace. */
		ps_queue_group_signal_unsafe(parent, task->life->exit_signal);
		if (parent->sched->status == ps_waiting)
			ps_put_to_ready_queue_unsafe(parent);
	}
out:
	spinlock_unlock(&ps_lock, irq);
}

void ps_put_to_wait_queue_unsafe(task_struct *task, list_entry *which_list,
				 const char *func)
{
	if (!which_list)
		which_list = &control.wait_queue;

	list_remove_entry(&task->sched->ps_list);

	if (task->life->psid != 0xffffffff)
		list_insert_tail(which_list, &task->sched->ps_list);

	task->wait->wait_interruptible = 0;
	task->sched->status = ps_waiting;
	task->wait->wait_func = func;
}

/* Move task to the wait queue (blocked on a lock or waitpid). */
void ps_put_to_wait_queue(task_struct *task, list_entry *which_list,
			  const char *func)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	ps_put_to_wait_queue_unsafe(task, which_list, func);
	spinlock_unlock(&ps_lock, irq);
}

void ps_put_to_ready_queue_unsafe(task_struct *task)
{
	if (task->sched->status == ps_dying)
		return;
	if (task->life->psid != 0xffffffff) {
		list_remove_entry(&task->sched->ps_list);
		list_insert_tail(&control.ready_queue[task->sched->priority],
				 &task->sched->ps_list);
	}
	task->wait->wait_interruptible = 0;
	task->sched->status = ps_ready;
	task->wait->wait_func = NULL;
}

/* Enqueue task in the ready queue at its current priority. */
void ps_put_to_ready_queue(task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	/*
	 * Public wakeups come from poll/select, locks, sockets, and signals.
	 * They should only transition tasks that are still blocked; otherwise
	 * a stale wakeup can relabel the current/runnable task as ps_ready.
	 */
	if (task->sched->status == ps_waiting)
		ps_put_to_ready_queue_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

int sched_enable()
{
	return ++current->sched->sched_level;
}

int sched_disable()
{
	return --current->sched->sched_level;
}

int sched_is_enabled()
{
	return current->sched->sched_level > 0;
}

/*
 * time_wait — sleep the current task for up to @ms milliseconds.
 *
 * If ms == 0 the task blocks indefinitely (no timer is armed); it can
 * only be woken by an external ps_put_to_ready_queue() call.
 *
 * The task is placed in the global wait queue.  Any caller that wants to
 * wake it early (e.g. an fd becoming readable) simply calls
 * ps_put_to_ready_queue(task).  The timer fires via ps_fire_timers_unsafe
 * on the next scheduling decision and does the same thing.
 */
void time_wait(unsigned ms)
{
	task_struct *cur = CURRENT_TASK();

	ps_prepare_timed_wait(cur, ms, __func__);
	task_sched();
	ps_finish_timed_wait(cur);
}

void ps_prepare_timed_wait(task_struct *task, unsigned ms, const char *func)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	if (ms > 0)
		timer_arm_unsafe(task, ms);
	ps_put_to_wait_queue_unsafe(task, NULL, func);
	spinlock_unlock(&ps_lock, irq);
}

int ps_prepare_interruptible_wait(task_struct *task, list_entry *queue,
				  unsigned ms, const char *func)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	if (ps_interrupting_signals(task) ||
	    (task->sighand &&
	     (ps_pending_signals(task) & task->wait->signal_wait_mask))) {
		spinlock_unlock(&ps_lock, irq);
		return -EINTR;
	}
	if (ms)
		timer_arm_unsafe(task, ms);
	ps_put_to_wait_queue_unsafe(task, queue, func);
	task->wait->wait_interruptible = 1;
	spinlock_unlock(&ps_lock, irq);
	return 0;
}

void ps_finish_timed_wait(task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	timer_disarm_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

/*
 * ps_signal_wait — atomically block until an unmasked signal is pending.
 *
 * Checks cur->signal->sig_pending & ~sig_mask under ps_lock so there is no
 * window between the test and the sleep where a signal can arrive and be lost.
 * Returns immediately if an unmasked signal is already pending.
 *
 * The caller is responsible for installing the desired mask in sig_mask before
 * calling and restoring the old mask after returning.
 */
void ps_signal_wait(void)
{
	task_struct *cur = CURRENT_TASK();

	while (!ps_interrupting_signals(cur)) {
		if (!ps_prepare_interruptible_wait(cur, NULL, 0, __func__)) {
			task_sched();
			ps_finish_timed_wait(cur);
		}
	}
}
