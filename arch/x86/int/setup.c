#include <arch/interrupt.h>
#include <config.h>
#include <int/int.h>
#include <macro.h>
#include <mm/mm.h>
#include <ps/ps.h>

void arch_interrupt_set_gate(int vector, vaddr_t entry, int trap, int dpl)
{
	idt[vector] = trap ? MAKE_TRAP_GATE(entry, dpl) :
			     MAKE_INTR_GATE(entry, dpl);
}

void arch_interrupt_activate(void)
{
	unsigned long long idtr = MAKE_IDTR_OPERAND(idt_size - 1, idt);
	unsigned long long gdtr = MAKE_GDTR_OPERAND(gdt_size - 1, gdt);

	SET_IDT(idtr);
	SET_GDT(gdtr);
	SET_CS(KERNEL_CODE_SELECTOR);
	SET_DS(KERNEL_DATA_SELECTOR);
}

void arch_interrupt_set_kernel_stack(void *address)
{
	unsigned base = (unsigned)address;

	gdt[TSS_SELECTOR / 8] = MAKE_SEG_DESC(base, TSS_SEG_LIMIT,
					      SEG_CLASS_SYSTEM, 9,
					      KERNEL_PRIVILEGE, SEG_BASE_1);
	SET_TSS(TSS_SELECTOR);
}

int arch_interrupt_frame_is_user(const intr_frame *frame)
{
	return (frame->cs & 3) == USER_PRIVILEGE;
}
