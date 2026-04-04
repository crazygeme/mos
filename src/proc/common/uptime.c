/*
 * /proc/uptime — system uptime and idle time.
 *
 * Format (Linux-compatible):
 *   <uptime_secs>.<centisecs> <idle_secs>.<centisecs>
 *
 * time_now_ms() goes through time_wall_us() which adds g_wall_offset_us,
 * returning calendar time rather than uptime.  time_now_tickets() is the
 * raw boot-relative jiffy counter (HZ=100, so 1 tick == 10 ms == 1 cs).
 */
#include "common.h"
#include <hw/time.h>
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
	unsigned long long ticks = time_now_tickets();
	unsigned up_sec = (unsigned)(ticks / HZ);
	unsigned up_cs = (unsigned)(ticks % HZ);

	idle_ctx_t ic = { 0 };
	ps_enum_all(sum_idle, &ic);
	unsigned long long it = ic.idle_ticks;
	unsigned idle_sec = (unsigned)(it / HZ);
	unsigned idle_cs = (unsigned)(it % HZ);

	proc_buf_printf(pb, "%u.%02u %u.%02u\n", up_sec, up_cs, idle_sec,
			idle_cs);
}

DEFINE_PROC_FILE(uptime, fill);
