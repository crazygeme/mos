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
#include <hw/time.h>
#include <mm/mm.h>
#include <int/int.h>
#include <int/dsr.h>
#include <lib/list.h>
#include <lib/klib.h>
#include <macro.h>

#include "ps_internal.h"

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
	unsigned now = time_now_ms();

	while (node != head) {
		task_struct *task = container_of(node, task_struct, ps_list);
		if (task->status != ps_dying && task->timeout <= now) {
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
	struct rb_root *root = &control.timer_queue;
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
	if (!RB_EMPTY_NODE(&task->timer_rb)) {
		rb_erase(&task->timer_rb, &control.timer_queue);
		RB_CLEAR_NODE(&task->timer_rb);
		task->timer_due_ms = 0;
	}
}

void ps_fire_timers_unsafe(void)
{
	unsigned now = time_now_ms();
	struct rb_node *n = rb_first(&control.timer_queue);

	while (n) {
		task_struct *t = rb_entry(n, task_struct, timer_rb);
		if (t->timer_due_ms > now)
			break;
		n = rb_next(n);
		rb_erase(&t->timer_rb, &control.timer_queue);
		RB_CLEAR_NODE(&t->timer_rb);
		t->timer_due_ms = 0;
		ps_put_to_ready_queue_unsafe(t);
	}
}

/* Scan ready levels from highest to lowest and return the next task to run. */
static task_struct *ps_get_next_task()
{
	task_struct *task = NULL;
	int i = PS_PRIORITY_MAX - 1;

	spinlock_lock(&ps_lock);
	ps_fire_timers_unsafe();
	for (; i >= 0; i--) {
		if (list_is_empty(&control.ready_queue[i]))
			continue;
		task = ps_get_available_ready_task(&control.ready_queue[i]);
		if (task)
			break;
	}
	spinlock_unlock(&ps_lock);
	return task;
}

/* Propagate kernel page-directory entries into task's saved page directory
 * so that any kernel mappings added since the task last ran become visible. */
static void ps_copy_kernel_map(task_struct *task)
{
	if (task->user->page_dir) {
		unsigned int cr3;
		unsigned int *in_use;
		unsigned int *per_ps = (unsigned int *)task->user->page_dir;

		LOAD_CR3(cr3);
		in_use = (unsigned int *)(cr3 + KERNEL_OFFSET);
		memcpy(&per_ps[KERNEL_PAGE_DIR_OFFSET],
		       &in_use[KERNEL_PAGE_DIR_OFFSET],
		       (1024 - KERNEL_PAGE_DIR_OFFSET) * sizeof(unsigned));
	}
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
	spinlock_lock(&ps_lock);
	ps_put_to_dying_queue_unsafe(task);
	if (task->parent) {
		/* Queue SIGCHLD before waking the parent so the signal is
		 * already pending when wait() returns to userspace. */
		task->parent->signal->sig_pending |= (1UL << (SIGCHLD - 1));
		ps_put_to_ready_queue_unsafe(task->parent);
	}
	spinlock_unlock(&ps_lock);
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
	spinlock_lock(&ps_lock);
	ps_put_to_wait_queue_unsafe(task, which_list, func);
	spinlock_unlock(&ps_lock);
}

void ps_put_to_ready_queue_unsafe(task_struct *task)
{
	if (task->psid != 0xffffffff) {
		list_remove_entry(&task->ps_list);
		list_insert_tail(&control.ready_queue[task->priority],
				 &task->ps_list);
	}
	task->status = ps_ready;
	task->wait_func = NULL;
}

/* Enqueue task in the ready queue at its current priority. */
void ps_put_to_ready_queue(task_struct *task)
{
	spinlock_lock(&ps_lock);
	ps_put_to_ready_queue_unsafe(task);
	spinlock_unlock(&ps_lock);
}

/*
 * Public — context switch
 */

/*
 * _task_sched — perform a voluntary or preemptive context switch.
 *
 * 1. Disable interrupts (switch must be atomic).
 * 2. Drain any pending DSR callbacks inline.
 * 3. Pick the next runnable task.
 * 4. If next == current: re-enable and return (no-op).
 * 5. Otherwise:
 *    a. SAVE_ALL: stash registers into current->tss; sets eip = NEXT label.
 *    b. RESTORE_ALL: load registers from next->tss.
 *    c. SET_CR3: switch address space.
 *    d. reset_tss: update hardware TSS for new task.
 *    e. JUMP_TO_NEXT_TASK_EIP: jump to next task's saved eip.
 *       On first run this is ps_run; on later runs it is the NEXT label.
 * 6. When rescheduled, execution resumes at NEXT.
 */
void _task_sched(const char *func)
{
	task_struct *task = 0;

	task_schedule_count++;

	CURRENT_TASK()->idle = time_now_tickets();

	dsr_drain();

	/* 
	 * Schedule procedure can not be interrupted.
	 */
	int_intr_disable();

	task = ps_get_next_task();

	if (!task || task->psid == CURRENT_TASK()->psid)
		goto SELF;

	CURRENT_TASK()->total_switches++;

	task->status = ps_running;
	/*
	 * Can be optimized by syncing pgd entry when adding/removing kernel mappings,
	 * but this is simpler and the overhead should be negligible.
	 */
	ps_copy_kernel_map(task);

	SAVE_ALL(CURRENT_TASK(), NEXT);

	/* Do TSS and CR3 setup on the current stack, before switching.
	 * Kernel mappings are shared across all page directories so SET_CR3
	 * here is safe — code and the current stack remain accessible. */
	reset_tss(task);
	SET_CR3(task->user->page_dir - KERNEL_OFFSET);

	/* Switch to the new task's kernel stack.  After this point no local
	 * variable may be written: CURRENT_TASK() derives the task from the
	 * updated ESP and the only stack write is the 4-byte return address
	 * pushed by the call below, at [new_esp - 4], which is within the
	 * task's page even when new_esp == task + PAGE_SIZE. */
	RESTORE_ALL(task, task->tss.eip);
	JUMP_TO_NEXT_TASK_EIP(CURRENT_TASK()->tss.eip);
	asm volatile("NEXT: nop");
SELF:
	int_intr_enable();

	CURRENT_TASK()->idle_tickets +=
		time_now_tickets() - CURRENT_TASK()->idle;
}

static int scheduler_enabled = 1;

int sched_enable()
{
	return __sync_lock_test_and_set(&scheduler_enabled, 1);
}

int sched_disable()
{
	return __sync_lock_test_and_set(&scheduler_enabled, 0);
}

int sched_set_level(int level)
{
	return __sync_lock_test_and_set(&scheduler_enabled, level);
}

int sched_is_enabled()
{
	return __sync_add_and_fetch(&scheduler_enabled, 0);
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

	spinlock_lock(&ps_lock);
	if (ms > 0)
		timer_arm_unsafe(cur, ms);
	ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
	spinlock_unlock(&ps_lock);
	task_sched();
	spinlock_lock(&ps_lock);
	timer_disarm_unsafe(cur);
	spinlock_unlock(&ps_lock);
}