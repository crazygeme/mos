#include <ps/ps.h>

/* Matches ps_context_switch: flags, edi, esi, ebx, ebp, return address. */
void arch_task_init_switch_frame(task_struct *task)
{
	unsigned *sp = (unsigned *)task->tss.esp;
	*--sp = task->tss.eip;
	*--sp = 0;
	*--sp = 0;
	*--sp = 0;
	*--sp = 0;
	*--sp = 2; /* IF remains clear until the new task entry is ready */
	task->switch_sp = (uintptr_t)sp;
	task->on_cpu = 0;
	task->terminate_requested = 0;
	task->sched_level = 1;
}
