#ifndef MOS_X86_ARCH_INTERRUPT_H
#define MOS_X86_ARCH_INTERRUPT_H

#include <arch/types.h>
#include <compiler.h>

/* Must match the pushes performed by arch/x86/int/impl/int.S. */
typedef struct _arch_intr_frame {
	unsigned int edi, esi, ebp, esp_dummy;
	unsigned int ebx, edx, ecx, eax;
	unsigned short gs, : 16;
	unsigned short fs, : 16;
	unsigned short es, : 16;
	unsigned short ds, : 16;
	unsigned int vec_no;
	unsigned int error_code;
	void *frame_pointer;
	void (*eip)(void);
	unsigned short cs, : 16;
	unsigned int eflags;
	void *esp;
	unsigned short ss, : 16;
} __attribute__((packed)) arch_intr_frame;

/* Generic kernel name retained for architecture-independent users. */
typedef arch_intr_frame intr_frame;

void arch_interrupt_set_gate(int vector, vaddr_t entry, int trap,
			     int dpl);
void arch_interrupt_activate(void);
void arch_interrupt_set_kernel_stack(void *address);
ALWAYS_INLINE int arch_interrupt_frame_is_user(const arch_intr_frame *frame)
{
	return (frame->cs & 3) != 0;
}

#endif
