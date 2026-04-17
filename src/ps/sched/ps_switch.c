#include <int/dsr.h>
#include <int/int.h>
#include <ps/ps.h>
#include "../ps_internal.h"
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
	task_struct *cur = CURRENT_TASK();

	__sync_fetch_and_add(&task_schedule_count, 1);

	if (cur->stats)
		cur->stats->idle = time_now_tickets();

	if (ps_sched_cpu() == 0)
		dsr_drain();

	/* 
	 * Schedule procedure can not be interrupted.
	 */
	sched_disable(ps_sched_cpu());

	task = ps_get_next_task();

	if (!task) {
		/*
		 * If the current task just blocked itself (for example in
		 * time_wait/select/poll), we must not fall straight back into
		 * it. Park the CPU until the next interrupt, then retry so
		 * expired timers can be promoted by ps_get_next_task().
		 */
		if (CURRENT_TASK()->status != ps_running) {
			sched_enable(ps_sched_cpu());
			int_intr_enable();
			PAUSE();
			int_intr_disable();
			goto SELF;
		}

		sched_enable(ps_sched_cpu());
		goto SELF;
	}

	if (task->psid == CURRENT_TASK()->psid) {
		sched_enable(ps_sched_cpu());
		goto SELF;
	}

	if (CURRENT_TASK()->stats)
		CURRENT_TASK()->stats->total_switches++;

	task->status = ps_running;
	SAVE_ALL(CURRENT_TASK(), NEXT);

	/* Do TSS and CR3 setup on the current stack, before switching.
	 * Kernel mappings are shared across all page directories so SET_CR3
	 * here is safe — code and the current stack remain accessible. */
	reset_tss(task);
	SET_CR3(task->user->page_dir - KERNEL_OFFSET);

	/* Reload per-process TLS descriptors so that when RESTORE_ALL
	 * loads GS the CPU finds the correct segment descriptor. */
	gdt[GDT_ENTRY_TLS_MIN + 0] = task->user->tls_desc[0];
	gdt[GDT_ENTRY_TLS_MIN + 1] = task->user->tls_desc[1];
	gdt[GDT_ENTRY_TLS_MIN + 2] = task->user->tls_desc[2];
	ps_update_ldt(task);

	/* Switch to the new task's kernel stack.  After this point no local
	 * variable may be written: CURRENT_TASK() derives the task from the
	 * updated ESP and the only stack write is the 4-byte return address
	 * pushed by the call below, at [new_esp - 4], which is within the
	 * task's page even when new_esp == task + PAGE_SIZE. */
	sched_enable(ps_sched_cpu());
	RESTORE_ALL(task, task->tss.eip);
	JUMP_TO_NEXT_TASK_EIP(CURRENT_TASK()->tss.eip);
	asm volatile("NEXT: nop");
SELF:

	if (CURRENT_TASK()->stats)
		CURRENT_TASK()->stats->idle_tickets +=
			time_now_tickets() - CURRENT_TASK()->stats->idle;
}
