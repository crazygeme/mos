#include <ps/cpu_local.h>
#include <config.h>
#include <lib/klib.h>
#include <macro.h>
#include <mm/mm.h>
#include <ps/smp.h>

extern void sysenter_entry(void);

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
	cpu->loaded_ldt_valid = 0;
	operand = MAKE_GDTR_OPERAND(sizeof(cpu->gdt) - 1, cpu->gdt);
	SET_GDT(operand);
	unsigned a, b, c, d;
	arch_cpu_cpuid(1, 0, &a, &b, &c, &d);
	cpu->sysenter_enabled = (d >> 11) & 1;
	if (cpu->sysenter_enabled) {
		cpu->gdt[SYSENTER_CODE_SELECTOR / 8] =
			cpu->gdt[KERNEL_CODE_SELECTOR / 8];
		cpu->gdt[SYSENTER_CODE_SELECTOR / 8 + 1] =
			cpu->gdt[KERNEL_DATA_SELECTOR / 8];
		arch_cpu_write_msr(0x174, SYSENTER_CODE_SELECTOR, 0);
		arch_cpu_write_msr(0x175, cpu->tss.tss.esp0, 0);
		arch_cpu_write_msr(0x176, (uintptr_t)sysenter_entry, 0);
	}
	asm volatile("movw %0, %%fs"
		     :
		     : "rm"((unsigned short)CPU_LOCAL_SELECTOR)
		     : "memory");
	operand = MAKE_IDTR_OPERAND(idt_size - 1, idt);
	arch_cpu_load_idt(&operand);
}
