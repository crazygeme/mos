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

/* Scheduling instrumentation. */
static unsigned long long sched_begin = 0;
static unsigned long long sched_end = 0;

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

/* Scan ready levels from highest to lowest and return the next task to run. */
static task_struct *ps_get_next_task()
{
	task_struct *task = NULL;
	int i = PS_PRIORITY_MAX - 1;

	spinlock_lock(&ps_lock);
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

/*
 * Static helpers — instrumentation and context-switch support
 */

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
		// FIXME(Ender): currently not working
		// task->parent->sig_pending |= (1UL << (SIGCHLD - 1));
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
	task_struct *current = 0;
	unsigned oldint;

	if (TestControl.profiling)
		sched_cal_begin();

	/* 
	 * Schedule procedure can not be interrupted.
	 */
	oldint = int_intr_disable();

	dsr_drain();

	current = CURRENT_TASK();
	task = ps_get_next_task();

	if (!task || task->psid == current->psid)
		goto SELF;

	current->niv_switches++;

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
	reset_tss(current);
	SET_CR3(current->user->page_dir - KERNEL_OFFSET);
	JUMP_TO_NEXT_TASK_EIP(current->tss.eip);
	asm volatile("NEXT: nop");
SELF:
	int_intr_setlevel(oldint);

	if (TestControl.profiling)
		sched_cal_end();
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