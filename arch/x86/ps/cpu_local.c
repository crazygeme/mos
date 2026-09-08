#include <arch/cpu_local.h>
#include <config.h>
#include <lib/klib.h>
#include <macro.h>
#include <mm/mm.h>
#include <ps/smp.h>

void arch_cpu_local_init(struct smp_cpu *cpu)
{
	unsigned long long operand;

	memcpy(cpu->gdt, gdt, sizeof(cpu->gdt));
	operand = MAKE_GDTR_OPERAND(sizeof(cpu->gdt) - 1, cpu->gdt);
	SET_GDT(operand);
	operand = MAKE_IDTR_OPERAND(idt_size - 1, idt);
	arch_cpu_load_idt(&operand);
	memset(&cpu->tss, 0, sizeof(cpu->tss));
	memset(cpu->tss.io_bitmap, 0xff, sizeof(cpu->tss.io_bitmap));
	cpu->tss.tss.ss0 = KERNEL_DATA_SELECTOR;
	cpu->tss.tss.iomap = offsetof(tss_io_struct, io_bitmap);
}
