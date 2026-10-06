#ifndef MOS_PS_USAGE_H
#define MOS_PS_USAGE_H

#include <ps/ps.h>

typedef struct {
	unsigned long long user __attribute__((aligned(8)));
	unsigned long long system, idle;
} cpu_usage_t;

/* Atomic snapshots also prevent torn 64-bit reads on i386. */
static inline unsigned long long ps_usage_read(unsigned long long *ticks)
{
	return __sync_fetch_and_add(ticks, 0);
}

void ps_usage_init(task_struct *task, task_struct *parent, int share);
void ps_usage_put(task_struct *task);
void ps_usage_charge(task_struct *task, cpu_usage_t *cpu, int user);
void ps_account_tick(intr_frame *frame);
void ps_cpu_usage(unsigned cpu, cpu_usage_t *usage);

#endif
