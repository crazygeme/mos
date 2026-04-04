/*
 * /proc/loadavg — system load averages.
 *
 * Format (Linux-compatible):
 *   <1min> <5min> <15min> <running>/<total> <last_pid>
 *
 * Load averages are EWMA of the number of runnable (running+ready) user tasks,
 * updated every LOAD_FREQ jiffies (5 s at HZ=100) using the same fixed-point
 * arithmetic as Linux (FSHIFT=11, FIXED_1=2048).
 *
 * The update is lazy: when fill() is called we replay every missed 5-second
 * window since the last update, using the current runnable count as the sample
 * for all of them.  This is the same approximation Linux uses when the update
 * tick fires late.
 */
#include "common.h"
#include <ps/ps.h>
#include <hw/time.h>

/* ── Fixed-point constants (identical to Linux) ─────────────────────── */

#define FSHIFT 11
#define FIXED_1 (1u << FSHIFT) /* 2048 == 1.0 */
#define LOAD_FREQ (5u * HZ) /* update interval: 500 jiffies */

#define EXP_1 1884u /* e^(-5/60)  * FIXED_1 */
#define EXP_5 2014u /* e^(-5/300) * FIXED_1 */
#define EXP_15 2037u /* e^(-5/900) * FIXED_1 */

#define LOAD_INT(x) ((x) >> FSHIFT)
#define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1 - 1u)) * 100u)

/* ── Global EWMA state ───────────────────────────────────────────────── */

static unsigned long avenrun[3]; /* fixed-point load averages */
static unsigned long long last_load_ticks; /* jiffies when last updated */

/* ── Helpers ─────────────────────────────────────────────────────────── */

static unsigned long calc_load(unsigned long load, unsigned long exp,
			       unsigned long active)
{
	unsigned long newload = load * exp + active * (FIXED_1 - exp);
	/* Round up when load is rising */
	if (active >= (load >> FSHIFT))
		newload += FIXED_1 - 1;
	return newload / FIXED_1;
}

typedef struct {
	unsigned total;
	unsigned running;
	unsigned active; /* running + ready non-idle user tasks */
	unsigned last_pid;
} loadavg_ctx_t;

static void count_task(task_struct *task, void *ctx)
{
	loadavg_ctx_t *c = (loadavg_ctx_t *)ctx;

	if (task->psid == 0xffffffff)
		return;
	if (task->type == ps_kernel || task->priority == ps_idle)
		return;

	c->total++;
	if (task->psid > c->last_pid)
		c->last_pid = task->psid;
	if (task->status == ps_running || task->status == ps_ready) {
		c->running++;
		c->active++;
	}
}

/* ── /proc/loadavg fill ──────────────────────────────────────────────── */

static void fill(proc_buf_t *pb)
{
	task_struct *cur = CURRENT_TASK();
	loadavg_ctx_t c = { 0, 0, 0, 0 };
	unsigned long long now = time_now_tickets();
	unsigned long long elapsed;

	ps_enum_all(count_task, &c);

	/* Initialise on first call. */
	if (last_load_ticks == 0)
		last_load_ticks = now;

	elapsed = now - last_load_ticks;

	/* Replay every missed 5-second window. */
	while (elapsed >= LOAD_FREQ) {
		avenrun[0] = calc_load(avenrun[0], EXP_1, c.active);
		avenrun[1] = calc_load(avenrun[1], EXP_5, c.active);
		avenrun[2] = calc_load(avenrun[2], EXP_15, c.active);
		last_load_ticks += LOAD_FREQ;
		elapsed -= LOAD_FREQ;
	}

	proc_buf_printf(pb, "%lu.%02lu %lu.%02lu %lu.%02lu %u/%u %u\n",
			LOAD_INT(avenrun[0]), LOAD_FRAC(avenrun[0]),
			LOAD_INT(avenrun[1]), LOAD_FRAC(avenrun[1]),
			LOAD_INT(avenrun[2]), LOAD_FRAC(avenrun[2]), c.running,
			c.total, c.last_pid ? c.last_pid : cur->psid);
}

DEFINE_PROC_FILE(loadavg, fill);
