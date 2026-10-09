#include <ps/ps.h>

/* Matches ps_context_switch: flags, edi, esi, ebx, ebp, return address. */
void arch_task_init_switch_frame(task_struct *task, uintptr_t ip,
				 uintptr_t stack)
{
	unsigned *sp = (unsigned *)stack;
	*--sp = ip;
	*--sp = 0;
	*--sp = 0;
	*--sp = 0;
	*--sp = 0;
	*--sp = 2; /* IF remains clear until the new task entry is ready */
	task->sched->switch_sp = (uintptr_t)sp;
	task->sched->on_cpu = 0;
	task->sched->terminate_requested = 0;
	task->sched->sched_level = 1;
}
