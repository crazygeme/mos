#include <ps/ps.h>
#include <int/int.h>
void arch_signal_deliver(task_struct *task, intr_frame *frame,
			 struct sigaction *action, int signal,
			 const struct signal_fault *fault)
{
	i386_signal_deliver(task, frame, action, signal, fault);
}
