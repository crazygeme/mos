#include <int/dsr.h>
#include <int/int.h>
#include <ps/ps.h>
#include <mm/mmap.h>
#include <ps/smp.h>
#include <mm/mmu.h>
#include <ps/task.h>
#include "../ps_internal.h"
/*
 * Public — context switch
 */

/*
 * Pick a runnable task and switch to it.
 *
 * The assembly switch saves an ordinary cdecl call frame. A resumed task
 * returns from that call and restores its own interrupt state.
 */
extern void ps_context_switch(unsigned *old_sp, unsigned new_sp);
extern void ps_reap_dead_threads(void);

void _task_sched(const char *func)
{
	task_struct *prev = current;
	task_struct *next;
	unsigned irq;
	unsigned cpu;

	(void)func;
	task_schedule_count++;

	if (prev->stats)
		prev->stats->idle = time_now_tickets();

	dsr_drain();

	irq = int_intr_disable();
	smp_kernel_enter();
	/* dead_threads is global; serialize its traversal and reclamation with
	 * exits and with other CPUs' schedulers under the BKL. */
	ps_reap_dead_threads();
	if (prev->status == ps_running)
		prev->status = ps_ready;

	next = ps_get_next_task();
	if (!next) DIE(); /* one permanent idle task per CPU */
	next->status = ps_running;

	if (next == prev) {
		ps_load_task_segments(next);
		int_intr_setlevel(irq);
		goto out;
	}

	if (prev->stats)
		prev->stats->total_switches++;

	smp_fpu_save(prev);

	/*
	 * Do TSS and CR3 setup on the current stack, before switching.
	 * Kernel mappings are shared across all address spaces, so activation here
	 * is safe: the code and current stack remain accessible.
	 */
	arch_task_activate(next);
	arch_mm_activate(VIRT_TO_PHY(next->user->vm->page_dir));

	/*
	 * Reload per-process TLS descriptors before the eventual user return.
	 */
	smp_fpu_restore(next);
	prev->on_cpu = 0;
	cpu = smp_cpu_id();
	next->on_cpu = cpu + 1;
	smp_cpus[cpu].task = next;
	/* No other CPU can select prev until next releases the BKL. */
	ps_context_switch(&prev->switch_sp, next->switch_sp);
	int_intr_setlevel(irq);

out:
	if (prev->stats)
		prev->stats->idle_tickets +=
			time_now_tickets() - prev->stats->idle;
}
