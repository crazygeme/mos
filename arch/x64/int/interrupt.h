#ifndef MOS_X64_ARCH_INTERRUPT_H
#define MOS_X64_ARCH_INTERRUPT_H
#include <arch/types.h>
#include <compiler.h>

/* Software save area followed by the five qwords pushed by IA-32e hardware.
 * Historical field names remain available to common process code; their
 * width is the kernel register width, independent of the process ABI. */
typedef struct _arch_intr_frame {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
	uint64_t gs, fs, es, ds;
	uint64_t vec_no, error_code;
	void *frame_pointer;
	void (*eip)(void);
	uint64_t cs, eflags;
	void *esp;
	uint64_t ss;
} arch_intr_frame;
typedef arch_intr_frame intr_frame;

struct arch_idt_gate {
	uint16_t offset_low, selector;
	uint8_t ist, attributes;
	uint16_t offset_mid;
	uint32_t offset_high, reserved;
} __attribute__((packed));
struct arch_descriptor_pointer {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));
_Static_assert(sizeof(struct arch_idt_gate) == 16, "IA-32e IDT gate size");
_Static_assert(sizeof(struct arch_descriptor_pointer) == 10,
	       "IA-32e table pointer size");
_Static_assert(sizeof(arch_intr_frame) == 224, "interrupt entry save layout");

/* Fast-return assembly compares these fields before restoring registers. */
_Static_assert(__builtin_offsetof(arch_intr_frame, r11) == 32 &&
		       __builtin_offsetof(arch_intr_frame, ecx) == 112 &&
		       __builtin_offsetof(arch_intr_frame, eip) == 184 &&
		       __builtin_offsetof(arch_intr_frame, cs) == 192 &&
		       __builtin_offsetof(arch_intr_frame, eflags) == 200 &&
		       __builtin_offsetof(arch_intr_frame, esp) == 208 &&
		       __builtin_offsetof(arch_intr_frame, ss) == 216,
	       "SYSRET frame offsets");

void arch_interrupt_set_gate(int vector, vaddr_t entry, int trap, int dpl);
void arch_interrupt_activate(void);
void arch_interrupt_set_kernel_stack(void *address);
ALWAYS_INLINE int arch_interrupt_frame_is_user(const arch_intr_frame *frame)
{
	return (frame->cs & 3) == 3;
}
/* i386 restores selectors on kernel exits too; AMD64 only on user exits. */
ALWAYS_INLINE int
arch_interrupt_frame_restores_segments(const arch_intr_frame *frame)
{
	return arch_interrupt_frame_is_user(frame);
}

#endif
