#ifndef MOS_X86_ARCH_TASK_H
#define MOS_X86_ARCH_TASK_H

struct _task_struct;
struct _intr_frame;

void arch_task_init(struct _task_struct *task);
void arch_task_activate(struct _task_struct *task);
void arch_task_reset_tls(struct _task_struct *task,
			 struct _intr_frame *frame);
void arch_task_init_user_frame(struct _intr_frame *frame, unsigned ip,
			       unsigned sp);

#endif
