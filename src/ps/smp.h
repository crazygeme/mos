#ifndef MOS_SMP_H
#define MOS_SMP_H

#include <ps/ps.h>

#define SMP_MAX_CPUS 32
#define SMP_TLB_VECTOR 0xf0
#define SMP_TICK_VECTOR 0xf1
#define SMP_SPURIOUS_VECTOR 0xff

struct smp_cpu {
	/* Kept first so arch_cpu_local() can load it from %fs:0. */
	struct smp_cpu *self;
	unsigned index;
	unsigned apic_id;
	volatile unsigned online;
	volatile unsigned tlb_ack;
	unsigned long long gdt[SELECTOR_COUNT];
	tss_io_struct tss;
	task_struct *task;
};

extern struct smp_cpu smp_cpus[SMP_MAX_CPUS];
extern volatile unsigned smp_online_count;
unsigned smp_cpu_id(void);
unsigned smp_cpu_count(void);
unsigned long long *smp_gdt(void);
tss_struct *smp_tss(void);
void smp_init(void);
void smp_bootstrap(void);
void smp_start(void);
int smp_kernel_enter(void);
void smp_kernel_leave(void);
void smp_return(intr_frame *frame);
void smp_idle(void);
void smp_tlb_flush(void);
void smp_tlb_poll(void);
int smp_interrupt(intr_frame *frame);
void smp_tick(void);
void smp_fpu_init(void);
void smp_fpu_save(task_struct *task);
void smp_fpu_restore(task_struct *task);
void smp_fpu_new(task_struct *task);
void smp_check_stop(void);

#endif
