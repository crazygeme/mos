#include <ps/ps.h>
extern void ret_from_fork(void);

void arch_task_init_switch_frame(task_struct *task, uintptr_t ip,
				 uintptr_t stack)
{
	/* C function entry needs rsp % 16 == 8 after ret. Fork return consumes
  * the interrupt frame directly, so its stack must not be rounded down. */

	if (ip != (uintptr_t)ret_from_fork)
		stack = (stack & ~(uintptr_t)15) - 8;
	uintptr_t *sp = (uintptr_t *)stack;
	*--sp = ip;
	for (unsigned i = 0; i < 6; i++)
		*--sp = 0;
	*--sp = 2;
	task->sched->switch_sp = (uintptr_t)sp;
	task->sched->on_cpu = 0;
	task->sched->terminate_requested = 0;
	task->sched->sched_level = 1;
}
