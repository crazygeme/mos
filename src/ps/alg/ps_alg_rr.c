/*
 * ps_sched.c — Multilevel Priority Ready Queue scheduler and context switch.
 *
 * Owns:
 *   - Ready/wait/dying queue transitions
 *   - MPRQ pick algorithm (RB-tree per priority level)
 *   - Context switch (_task_sched)
 *   - Scheduling instrumentation
 */

#include <ps/ps.h>
#include <ps/signal.h>
#include <hw/cpu.h>
#include <hw/time.h>
#include <mm/mm.h>
#include <int/int.h>
#include <int/dsr.h>
#include <lib/list.h>
#include <lib/klib.h>
#include <macro.h>
#include <config.h>

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
static list_entry *ps_ready_queue_head(unsigned cpu, int priority)
{
	return &control.cpu[cpu].ready_queue[priority];
}

static struct rb_root *ps_timer_queue_root(unsigned cpu)
{
	return &control.cpu[cpu].timer_queue;
}

static task_struct *ps_get_available_ready_task(list_entry *head)
{
	list_entry *node = head->next;

	while (node != head) {
		task_struct *task = container_of(node, task_struct, ps_list);
		if (task->status != ps_dying) {
			list_remove_entry(node);
			list_insert_tail(head, &task->ps_list);
			return task;
		}
		node = node->next;
	}
	return NULL;
}

/*
 * Timer queue helpers — all called with ps_lock held.
 *
 * timer_arm_unsafe: insert @task into the per-CPU timer RB-tree with
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
	unsigned cpu = ps_task_cpu(task);
	struct rb_root *root = ps_timer_queue_root(cpu);
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;
	unsigned due = time_now_ms() + ms;

	task->timer_due_ms = due;
	while (*link) {
		task_struct *t = rb_entry(*link, task_struct, timer_rb);
		parent = *link;
		if (due < t->timer_due_ms)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&task->timer_rb, parent, link);
	rb_insert_color(&task->timer_rb, root);
}

void timer_disarm_unsafe(task_struct *task)
{
	unsigned cpu = ps_task_cpu(task);

	if (!RB_EMPTY_NODE(&task->timer_rb)) {
		rb_erase(&task->timer_rb, ps_timer_queue_root(cpu));
		RB_CLEAR_NODE(&task->timer_rb);
		task->timer_due_ms = 0;
	}
}

void ps_fire_timers_unsafe(void)
{
	unsigned cpu = cpu_current_id();
	unsigned now = time_now_ms();
	struct rb_node *n = rb_first(ps_timer_queue_root(cpu));

	while (n) {
		task_struct *t = rb_entry(n, task_struct, timer_rb);
		if (t->timer_due_ms > now)
			break;
		n = rb_next(n);
		rb_erase(&t->timer_rb, ps_timer_queue_root(cpu));
		RB_CLEAR_NODE(&t->timer_rb);
		t->timer_due_ms = 0;
		ps_put_to_ready_queue_unsafe(t);
	}
}

/* Scan ready levels from highest to lowest and return the next task to run. */
task_struct *ps_get_next_task()
{
	task_struct *task = NULL;
	unsigned cpu = cpu_current_id();
	int i = PS_PRIORITY_MAX - 1;
	int irq;

	spinlock_lock(&ps_lock, &irq);
	if (cpu == 0 && current->psid != 0xffffffff)
		ps_fire_timers_unsafe();
	for (; i >= 0; i--) {
		list_entry *head = ps_ready_queue_head(cpu, i);
		if (list_is_empty(head))
			continue;
		task = ps_get_available_ready_task(head);
		if (task)
			break;
	}
	if (!task && ps_migrate_steal_unsafe(cpu)) {
		for (i = PS_PRIORITY_MAX - 1; i >= 0; i--) {
			list_entry *head = ps_ready_queue_head(cpu, i);
			if (list_is_empty(head))
				continue;
			task = ps_get_available_ready_task(head);
			if (task)
				break;
		}
	}
	spinlock_unlock(&ps_lock, irq);
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
	list_remove_entry(&task->ps_list);

	if (task->psid != 0xffffffff)
		list_insert_tail(&control.dying_queue, &task->ps_list);

	task->status = ps_dying;
	task->wait_func = NULL;
}

void ps_put_to_dying_queue(task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	ps_put_to_dying_queue_unsafe(task);
	if (task->ppid && task->exit_signal > 0 && task->exit_signal < NSIG) {
		task_struct *parent = ps_find_process_unsafe(task->ppid);

		if (!parent)
			goto out;
		/* Queue the requested exit signal before waking the parent so the
		 * already pending when wait() returns to userspace. */
		parent->signal->sig_pending |= (1UL << (task->exit_signal - 1));
		if (parent->status == ps_waiting)
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

	list_remove_entry(&task->ps_list);

	if (task->psid != 0xffffffff)
		list_insert_tail(which_list, &task->ps_list);

	task->status = ps_waiting;
	task->wait_func = func;
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
	unsigned dst_cpu;
	unsigned src_cpu = ps_current_cpu_early();

	if (task->psid != 0xffffffff) {
		dst_cpu = ps_migrate_select_cpu_unsafe(task, src_cpu);
		task->affinity = dst_cpu;
		list_remove_entry(&task->ps_list);
		list_insert_tail(ps_ready_queue_head(dst_cpu, task->priority),
				 &task->ps_list);
	} else {
		dst_cpu = ps_task_cpu(task);
	}
	task->status = ps_ready;
	task->wait_func = NULL;
	ps_kick_cpu_if_needed(dst_cpu, src_cpu);
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
	if (task->status == ps_waiting)
		ps_put_to_ready_queue_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

static int scheduler_enabled[MAX_CPUS] = { [0 ... MAX_CPUS - 1] = 1 };

int sched_enable(unsigned cpu)
{
	return __sync_add_and_fetch(&scheduler_enabled[cpu], 1);
}

int sched_disable(unsigned cpu)
{
	return __sync_add_and_fetch(&scheduler_enabled[cpu], -1);
}

int sched_is_enabled(unsigned cpu)
{
	return __sync_add_and_fetch(&scheduler_enabled[cpu], 0) > 0;
}

int sched_set_level(unsigned cpu, int level)
{
	scheduler_enabled[cpu] = level;
	return level;
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
	int irq;

	spinlock_lock(&ps_lock, &irq);
	if (ms > 0)
		timer_arm_unsafe(cur, ms);
	ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
	spinlock_unlock(&ps_lock, irq);
	task_sched();
	spinlock_lock(&ps_lock, &irq);
	timer_disarm_unsafe(cur);
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
	int irq;

	spinlock_lock(&ps_lock, &irq);
	if (!(cur->signal->sig_pending & ~cur->signal->sig_mask)) {
		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
		spinlock_unlock(&ps_lock, irq);
		task_sched();
	} else {
		spinlock_unlock(&ps_lock, irq);
	}
}
