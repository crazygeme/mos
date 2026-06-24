#include <int/dsr.h>
#include <int/int.h>
#include <ps/ps.h>
#include "../ps_internal.h"
/*
 * Public — context switch
 */

/*
 * Pick a runnable task and switch to it.
 *
 * SAVE_ALL records the resume label in prev->tss.eip.  A task returning from
 * a later context switch therefore resumes at task_resume and completes the
 * accounting for the interval during which it was not running.
 */
void _task_sched(const char *func)
{
	task_struct *prev = current;
	task_struct *next;

	(void)func;
	task_schedule_count++;

	if (prev->stats)
		prev->stats->idle = time_now_tickets();

	dsr_drain();

	/* Task selection and the context switch form one atomic operation. */
	sched_disable();

	next = ps_get_next_task();
	next->status = ps_running;

	if (next == prev) {
		ps_load_task_segments(next);
		sched_enable();
		goto out;
	}

	if (prev->stats)
		prev->stats->total_switches++;

	SAVE_ALL(prev, task_resume);

	/*
	 * Do TSS and CR3 setup on the current stack, before switching.
	 * Kernel mappings are shared across all page directories so SET_CR3
	 * here is safe: the code and current stack remain accessible.
	 */
	reset_tss(next);
	SET_CR3(VIRT_TO_PHY(next->user->page_dir));

	/*
	 * Reload per-process TLS descriptors before RESTORE_ALL loads GS.
	 */
	ps_load_task_segments(next);

	/*
	 * Switch to the new task's kernel stack.  After this point no local
	 * variable may be written: CURRENT_TASK() derives the task from the
	 * updated ESP and the only stack write is the 4-byte return address
	 * pushed by the call below, at [new_esp - 4], which is within the
	 * task's page even when new_esp == task + PAGE_SIZE.
	 */
	RESTORE_ALL(next, next->tss.eip);
	sched_enable();
	JUMP_TO_NEXT_TASK_EIP(current->tss.eip);
	asm volatile("task_resume: nop");

out:
	if (prev->stats)
		prev->stats->idle_tickets +=
			time_now_tickets() - prev->stats->idle;
}
