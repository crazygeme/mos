#ifndef MOS_X86_ARCH_INTERRUPT_H
#define MOS_X86_ARCH_INTERRUPT_H

struct _intr_frame;

void arch_interrupt_set_gate(int vector, unsigned long entry, int trap,
			     int dpl);
void arch_interrupt_activate(void);
void arch_interrupt_set_kernel_stack(void *address);
int arch_interrupt_frame_is_user(const struct _intr_frame *frame);

#endif
