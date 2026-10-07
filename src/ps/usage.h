#ifndef MOS_PS_USAGE_H
#define MOS_PS_USAGE_H

#include <ps/ps.h>

typedef struct {
	ps_tick_t user, system, idle;
} cpu_ticks_t;

/* Widen snapshots before summing CPUs or converting to time units. */
typedef struct {
	unsigned long long user;
	unsigned long long system, idle;
} cpu_usage_t;

/* Native-width loads are untorn and do not lock the cache line. */
static inline unsigned long long ps_usage_read(const ps_tick_t *ticks)
{
	return __atomic_load_n(ticks, __ATOMIC_RELAXED);
}

/* Only for one writer (local timer), or writers serialized by ps_lock. */
static inline void ps_usage_add_local(ps_tick_t *ticks, ps_tick_t delta)
{
	ps_tick_t value = __atomic_load_n(ticks, __ATOMIC_RELAXED);
	__atomic_store_n(ticks, value + delta, __ATOMIC_RELAXED);
}

void ps_usage_init(task_struct *task, task_struct *parent, int share);
void ps_usage_put(task_struct *task);
void ps_usage_charge(task_struct *task, cpu_ticks_t *cpu, int user);
void ps_account_tick(intr_frame *frame);
void ps_cpu_usage(unsigned cpu, cpu_usage_t *usage);

#endif
