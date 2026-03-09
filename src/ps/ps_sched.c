/*
 * ps_sched.c — Multilevel Priority Ready Queue scheduler and context switch.
 *
 * Owns:
 *   - Ready/wait/dying queue transitions
 *   - MPRQ pick algorithm (RB-tree per priority level)
 *   - Context switch (_task_sched)
 *   - Scheduling instrumentation
 */

#include <time.h>
#include <mm.h>
#include <klib.h>
#include <int.h>
#include <macro.h>
#include <dsr.h>
#include "ps_internal.h"

/* -------------------------------------------------------------------------
 * Static globals
 * ------------------------------------------------------------------------- */

/* Monotonically increasing tick; lower sched_seq = older = runs first. */
static unsigned long sched_clock = 0;

/* Scheduling instrumentation. */
static unsigned long long sched_begin = 0;
static unsigned long long sched_end = 0;

/* -------------------------------------------------------------------------
 * Static helpers — MPRQ algorithm
 * ------------------------------------------------------------------------- */

/* Insert task into a per-priority RB-tree ordered by sched_seq.
 * Must be called with ps_lock held. */
static void ready_queue_insert(struct rb_root *root, task_struct *task)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;

	while (*link) {
		task_struct *t = rb_entry(*link, task_struct, rb_node);
		parent = *link;
		link = (task->sched_seq < t->sched_seq) ? &(*link)->rb_left :
							  &(*link)->rb_right;
	}
	rb_link_node(&task->rb_node, parent, link);
	rb_insert_color(&task->rb_node, root);
}

/* Remove task from its priority-level RB-tree if currently enqueued.
 * Must be called with ps_lock held. */
static void ps_remove_from_ready(task_struct *task)
{
	if (!RB_EMPTY_NODE(&task->rb_node)) {
		rb_erase(&task->rb_node, &control.ready_queue[task->priority]);
		RB_CLEAR_NODE(&task->rb_node);
	}
}

/* Return the first runnable task from the given RB-tree.
 * Skips sleeping (timeout in the future) and dying tasks.
 * Re-inserts the chosen task with a fresh sched_seq for round-robin fairness.
 * Must be called with ps_lock held. */
static task_struct *ps_get_available_ready_task(struct rb_root *root)
{
	struct rb_node *node = rb_first(root);
	unsigned now = time_now_ms();

	while (node) {
		task_struct *task = rb_entry(node, task_struct, rb_node);
		if (task->status != ps_dying && task->timeout <= now) {
			rb_erase(node, root);
			task->sched_seq = ++sched_clock;
			RB_CLEAR_NODE(&task->rb_node);
			ready_queue_insert(root, task);
			return task;
		}
		node = rb_next(node);
	}
	return NULL;
}

/* Scan ready levels from highest to lowest and return the next task to run. */
static task_struct *ps_get_next_task()
{
	task_struct *task = NULL;
	int i = PS_PRIORITY_MAX - 1;

	spinlock_lock(&ps_lock);
	for (; i >= 0; i--) {
		if (RB_EMPTY_ROOT(&control.ready_queue[i]))
			continue;
		task = ps_get_available_ready_task(&control.ready_queue[i]);
		if (task)
			break;
	}
	spinlock_unlock(&ps_lock);
	return task;
}

/* -------------------------------------------------------------------------
 * Static helpers — instrumentation and context-switch support
 * ------------------------------------------------------------------------- */

static void sched_cal_begin()
{
	sched_begin = time_now_us();
}

static void sched_cal_end()
{
	sched_end = time_now_us();
	if (sched_begin)
		task_schedule_time += sched_end - sched_begin;
	sched_begin = sched_end = 0;
	task_schedule_count++;
}

/* Propagate kernel page-directory entries into task's saved page directory
 * so that any kernel mappings added since the task last ran become visible. */
static void ps_save_kernel_map(task_struct *task)
{
	if (task->user.page_dir) {
		unsigned int cr3;
		unsigned int *in_use;
		unsigned int *per_ps = (unsigned int *)task->user.page_dir;

		LOAD_CR3(cr3);
		in_use = (unsigned int *)(cr3 + KERNEL_OFFSET);
		memcpy(&per_ps[KERNEL_PAGE_DIR_OFFSET],
		       &in_use[KERNEL_PAGE_DIR_OFFSET],
		       (1024 - KERNEL_PAGE_DIR_OFFSET) * sizeof(unsigned));
	}
}

/* Wake the process with the given pid by moving it to the ready queue. */
static void ps_notify_process(unsigned pid)
{
	task_struct *task = ps_find_process(pid);
	if (task)
		ps_put_to_ready_queue(task);
}

/* -------------------------------------------------------------------------
 * Public — queue transitions
 * ------------------------------------------------------------------------- */

/* Move task to the dying queue and notify its parent.
 * Status is set to ps_dying only after enqueueing so a preemption between the
 * two steps cannot lose the task (the scheduler skips ps_dying tasks). */
void ps_put_to_dying_queue(task_struct *task)
{
	spinlock_lock(&ps_lock);

	ps_remove_from_ready(task);

	if (task->ps_list.prev && task->ps_list.next)
		list_remove_entry(&task->ps_list);

	if (task->psid != 0xffffffff)
		list_insert_tail(&control.dying_queue, &task->ps_list);

	task->status = ps_dying;

	spinlock_unlock(&ps_lock);

	ps_notify_process(task->parent);
}

/* Move task to the wait queue (blocked on a lock or waitpid). */
void ps_put_to_wait_queue(task_struct *task, list_entry *which_list)
{
	spinlock_lock(&ps_lock);

	if (!which_list)
		which_list = &control.wait_queue;

	ps_remove_from_ready(task);

	if (task->ps_list.prev && task->ps_list.next)
		list_remove_entry(&task->ps_list);

	if (task->psid != 0xffffffff)
		list_insert_tail(which_list, &task->ps_list);

	spinlock_unlock(&ps_lock);
}

/* Enqueue task in the ready queue at its current priority. */
void ps_put_to_ready_queue(task_struct *task)
{
	spinlock_lock(&ps_lock);
	if (task->psid != 0xffffffff) {
		if (task->ps_list.prev && task->ps_list.next)
			list_remove_entry(&task->ps_list);
		if (!RB_EMPTY_NODE(&task->rb_node))
			rb_erase(&task->rb_node,
				 &control.ready_queue[task->priority]);
		task->sched_seq = ++sched_clock;
		RB_CLEAR_NODE(&task->rb_node);
		ready_queue_insert(&control.ready_queue[task->priority], task);
	}
	spinlock_unlock(&ps_lock);
}

/* -------------------------------------------------------------------------
 * Public — context switch
 * ------------------------------------------------------------------------- */

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
	task_struct *current = 0;

	if (TestControl.profiling)
		sched_cal_begin();

	int_intr_disable();

	dsr_drain();

	current = CURRENT_TASK();
	current->is_switching = 1;
	task = ps_get_next_task();

	if (!task || task->psid == current->psid)
		goto SELF;

	task->status = ps_running;
	/*
	 * Actually can be optimized by syncing pgd entry when adding/removing kernel mappings, 
	 * but this is simpler and the overhead should be negligible.
	 */
	ps_save_kernel_map(task);
	SAVE_ALL(current, NEXT);

	/* After RESTORE_ALL the old stack is gone; CURRENT_TASK() reads the
	 * new task from the updated ESP — no globals needed. */
	RESTORE_ALL(task, task->tss.eip);
	current = CURRENT_TASK();
	SET_CR3(current->user.page_dir - KERNEL_OFFSET);
	reset_tss(current);
	JUMP_TO_NEXT_TASK_EIP(current->tss.eip);
	asm volatile("NEXT: nop");
SELF:
	int_intr_enable();
	current->is_switching = 0;
	if (TestControl.profiling)
		sched_cal_end();
}
