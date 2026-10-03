#include <ps/cpu_local.h>
#include <ps/smp.h>
#include <mm/mm.h>
#include <lib/klib.h>
#include <int/interrupt.h>
#include <macro.h>

static unsigned char critical_stacks[SMP_MAX_CPUS][3][4096]
	__attribute__((aligned(16)));
_Static_assert(__builtin_offsetof(struct smp_cpu, syscall_sp) == 8,
	       "SYSCALL stack offset");
_Static_assert(__builtin_offsetof(struct smp_cpu, user_sp) == 16,
	       "SYSCALL saved rsp offset");
extern void native_syscall_entry(void);
void arch_cpu_local_init(struct smp_cpu *cpu)
{
	arch_cpu_write_msr(0xc0000101, (uint32_t)(uintptr_t)cpu,
			   (uint32_t)((uintptr_t)cpu >> 32));
	arch_cpu_write_msr(0xc0000102, 0, 0);
	if (cpu->self != cpu) {
		memcpy(cpu->gdt, gdt, sizeof(cpu->gdt));
		memset(&cpu->tss, 0, sizeof(cpu->tss));
		memset(cpu->tss.io_bitmap, 0xff, sizeof(cpu->tss.io_bitmap));
		cpu->tss.tss.iomap =
			__builtin_offsetof(tss_io_struct, io_bitmap);
		for (unsigned i = 0; i < 3; i++)
			cpu->tss.tss.ist[i] =
				(uintptr_t)&critical_stacks[cpu->index][i][4096];
		cpu->self = cpu;
	}
	struct arch_descriptor_pointer gdtr = {
		.limit = sizeof(cpu->gdt) - 1,
		.base = (uintptr_t)cpu->gdt,
	};
	unsigned low, high;
	arch_cpu_read_msr(0xc0000080, &low, &high);
	arch_cpu_write_msr(0xc0000080, low | 1, high);
	arch_cpu_write_msr(0xc0000081, 0, KERNEL_CODE_SELECTOR);
	uintptr_t entry = (uintptr_t)native_syscall_entry;
	arch_cpu_write_msr(0xc0000082, entry, entry >> 32);
	arch_cpu_write_msr(0xc0000084, 0x47700,
			   0); /* clear IF, TF, DF, IOPL, NT, AC */
	struct arch_descriptor_pointer idtr = { idt_size - 1, (uintptr_t)idt };
	asm volatile("lgdt %0; lidt %1" : : "m"(gdtr), "m"(idtr) : "memory");
	arch_interrupt_set_kernel_stack((void *)&cpu->tss.tss);
}
