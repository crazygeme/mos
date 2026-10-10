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
extern void ps_context_switch(uintptr_t *old_sp, uintptr_t new_sp,
			      task_struct *prev);
extern void ps_reap_dead_threads(void);

void task_sched(void)
{
	task_struct *prev = current;
	task_struct *next;
	unsigned irq;
	unsigned cpu;
	int lock_irq;

	__atomic_fetch_add(&task_schedule_count, 1, __ATOMIC_RELAXED);

	irq = int_intr_disable();
	ps_reap_dead_threads();
	spinlock_lock(&ps_lock, &lock_irq);
	if (prev->sched->status == ps_running)
		prev->sched->status = ps_ready;

	next = ps_get_next_task_unsafe();
	if (!next)
		DIE(); /* one permanent idle task per CPU */
	next->sched->status = ps_running;

	if (next == prev) {
		spinlock_unlock(&ps_lock, lock_irq);
		ps_load_task_segments(next);
		int_intr_setlevel(irq);
		return;
	}

	if (prev->stats)
		prev->stats->total_switches++;

	/* Read native bases once at a real switch, not at each interrupt. */
	arch_task_save_user_segments(prev);
	smp_fpu_save(prev);

	/*
	 * Do TSS and CR3 setup on the current stack, before switching.
	 * Kernel mappings are shared across all address spaces, so activation here
	 * is safe: the code and current stack remain accessible.
	 */
	arch_task_activate(next);
	smp_mm_activate(VIRT_TO_PHY(next->memory->page_dir));

	/*
	 * Reload per-process TLS descriptors before the eventual user return.
	 */
	smp_fpu_restore(next);
	cpu = smp_cpu_id();
	next->sched->on_cpu = cpu + 1;
	smp_cpus[cpu].task = next;
	/* Release the scheduler lock only after the outgoing stack is inactive. */
	ps_context_switch(&prev->sched->switch_sp, next->sched->switch_sp,
			  prev);
	int_intr_setlevel(irq);
}

/* Invoked by assembly on the incoming stack with local interrupts disabled. */
void ps_switch_finish(task_struct *prev)
{
	prev->sched->on_cpu = 0;
	/* A parent can sleep after observing a zombie whose stack is still active. */
	if (prev->sched->status == ps_dying && prev->life->psid != 0xffffffff) {
		task_struct *parent = ps_find_process_unsafe(prev->life->ppid);
		if (parent && parent->sched->status == ps_waiting)
			ps_put_to_ready_queue_unsafe(parent);
	}
	spinlock_unlock(&ps_lock, 0);
}
