#ifndef MOS_X86_ARCH_CPU_LOCAL_H
#define MOS_X86_ARCH_CPU_LOCAL_H

struct smp_cpu;
void arch_cpu_local_init(struct smp_cpu *cpu);

static inline struct smp_cpu *arch_cpu_local(void)
{
	struct smp_cpu *cpu;
	asm volatile("movl %%fs:0, %0" : "=r"(cpu));
	return cpu;
}

#endif
