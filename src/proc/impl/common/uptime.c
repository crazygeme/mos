/*
 * /proc/uptime — system uptime and idle time.
 *
 * Format (Linux-compatible):
 *   <uptime_secs>.<centisecs> <idle_secs>.<centisecs>
 *
 * Uptime follows the monotonic clock, including delayed timer interrupts.
 */
#include "common.h"
#include <device/time.h>
#include <ps/ps.h>

typedef struct {
	unsigned long long idle_ticks;
} idle_ctx_t;

static void sum_idle(task_struct *task, void *ctx)
{
	idle_ctx_t *c = ctx;
	if (task->psid == 0xffffffff || !task->stats)
		return;
	c->idle_ticks += task->stats->idle_tickets;
}

static void fill(proc_buf_t *pb)
{
	unsigned long long ms = time_now_ms();
	unsigned up_sec = (unsigned)(ms / 1000);
	unsigned up_cs = (unsigned)((ms % 1000) / 10);

	idle_ctx_t ic = { 0 };
	ps_enum_all(sum_idle, &ic);
	unsigned long long it = ic.idle_ticks;
	unsigned idle_sec = (unsigned)(it / HZ);
	unsigned idle_cs = (unsigned)(it % HZ);

	proc_buf_printf(pb, "%u.%02u %u.%02u\n", up_sec, up_cs, idle_sec,
			idle_cs);
}

DEFINE_PROC_FILE(uptime, fill);
