#include <int/interrupt.h>
#include <mm/mm.h>
#include <ps/smp.h>
#include <config.h>
#include <macro.h>
#include <lib/klib.h>

/* The common IDT declaration is an array of qwords; IA-32e uses two per gate. */
void arch_interrupt_set_gate(int vector, vaddr_t entry, int trap, int dpl)
{
	struct arch_idt_gate *gate = &((struct arch_idt_gate *)idt)[vector];
	*gate = (struct arch_idt_gate){
		.ist = vector == 2  ? 1 :
		       vector == 8  ? 2 :
		       vector == 18 ? 3 :
				      0,
		.offset_low = entry,
		.selector = KERNEL_CODE_SELECTOR,
		.attributes = (entry ? 0x80 : 0) | ((dpl & 3) << 5) |
			      (trap ? 15 : 14),
		.offset_mid = entry >> 16,
		.offset_high = entry >> 32,
	};
}
void arch_interrupt_activate(void)
{
	struct arch_descriptor_pointer gdtr = {
		.limit = gdt_size - 1,
		.base = (uintptr_t)smp_gdt(),
	};
	struct arch_descriptor_pointer idtr = {
		.limit = idt_size - 1,
		.base = (uintptr_t)idt,
	};
	asm volatile("lgdt %0; lidt %1" : : "m"(gdtr), "m"(idtr) : "memory");
}
void arch_interrupt_set_kernel_stack(void *address)
{
	uintptr_t base = (uintptr_t)address;
	uint64_t *table = (uint64_t *)smp_gdt();
	/* IA-32e TSS and LDT system descriptors occupy two GDT slots. */
	table[TSS_SELECTOR / 8] = (TSS_SEG_LIMIT & 0xffff) |
				  ((base & 0xffffff) << 16) | (0x89ULL << 40) |
				  ((TSS_SEG_LIMIT & 0xf0000ULL) << 32) |
				  ((base & 0xff000000ULL) << 32);
	table[TSS_SELECTOR / 8 + 1] = base >> 32;
	asm volatile("ltr %w0" : : "r"((uint16_t)TSS_SELECTOR) : "memory");
}
