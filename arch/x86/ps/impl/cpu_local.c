#include <ps/cpu_local.h>
#include <config.h>
#include <lib/klib.h>
#include <macro.h>
#include <mm/mm.h>
#include <ps/smp.h>

void arch_cpu_local_init(struct smp_cpu *cpu)
{
	unsigned long long operand;
	unsigned limit = sizeof(*cpu) - 1;

	if (cpu->self != cpu) {
		memcpy(cpu->gdt, gdt, sizeof(cpu->gdt));
		memset(&cpu->tss, 0, sizeof(cpu->tss));
		memset(cpu->tss.io_bitmap, 0xff, sizeof(cpu->tss.io_bitmap));
		cpu->tss.tss.ss0 = KERNEL_DATA_SELECTOR;
		cpu->tss.tss.iomap = offsetof(tss_io_struct, io_bitmap);
		cpu->self = cpu;
	}
	cpu->gdt[CPU_LOCAL_SELECTOR / 8] =
		MAKE_SEG_DESC((unsigned)cpu, limit, SEG_CLASS_DATA, 2,
			      KERNEL_PRIVILEGE, SEG_BASE_1);
	operand = MAKE_GDTR_OPERAND(sizeof(cpu->gdt) - 1, cpu->gdt);
	SET_GDT(operand);
	asm volatile("movw %0, %%fs" : : "rm"((unsigned short)CPU_LOCAL_SELECTOR)
		     : "memory");
	operand = MAKE_IDTR_OPERAND(idt_size - 1, idt);
	arch_cpu_load_idt(&operand);
}
