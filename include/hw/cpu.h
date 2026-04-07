#ifndef _HW_CPU_H_
#define _HW_CPU_H_

#include <config.h>
#include <ps/ps.h>

/*
 * Per-CPU state.  One instance in the global cpus[] array for each
 * processor that comes online.  cpu_id 0 is always the BSP.
 */
typedef struct _cpu_struct {
	int cpu_id; /* logical CPU index (0 = BSP) */
	unsigned char apic_id; /* hardware Local APIC ID */
	volatile int online; /* set to 1 by AP after initialisation */
	tss_struct *tss; /* per-CPU TSS (allocated from kernel heap) */
	task_struct *idle; /* per-CPU idle task */
} cpu_struct;

extern cpu_struct cpus[MAX_CPUS];
extern volatile int ncpus; /* number of CPUs online */

/* Return the cpu_struct for the currently executing CPU.
 * Reads the APIC ID from the LAPIC MMIO register. */
cpu_struct *cpu_current(void);

/* Return logical CPU id of the current CPU (0 = BSP). */
int cpu_current_id(void);

int cpu_id(int index);

/* Bring all Application Processors online.
 * Must be called after apic_init_bsp() and ioapic_init(). */
void smp_start_aps(void);

/* Called by each AP from C code after protected-mode/paging setup. */
void ap_init_c(int cpu_id);

/* Broadcast a TLB invalidation IPI to all other online CPUs. */
void smp_tlb_shootdown(void);

/* Release APs into the scheduler after BSP task bootstrap is ready. */
void smp_scheduler_start(void);

/* Initialise the BSP's entry in cpus[] (called once from stage2). */
void cpu_init_bsp(void);

#endif /* _CPU_H_ */
