/*
 * /proc/loadavg — system load averages.
 *
 * Format (Linux-compatible):
 *   <1min> <5min> <15min> <running>/<total> <last_pid>
 *
 * Load averages require an EWMA updated on a timer; we report 0.00 for all
 * three.  Running and total task counts are derived from ps_enum_all().
 */
#include "generic.h"
#include <ps/ps.h>

typedef struct {
	unsigned total;
	unsigned running;
} loadavg_ctx_t;

static void count_task(task_struct *task, void *ctx)
{
	loadavg_ctx_t *c = (loadavg_ctx_t *)ctx;
	c->total++;
	if (task->status == ps_running)
		c->running++;
}

static void fill(void *buf, size_t size)
{
	task_struct *cur = CURRENT_TASK();
	loadavg_ctx_t c = { 0, 0 };
	ps_enum_all(count_task, &c);

	memset(buf, 0, size);
	sprintf(buf, "0.00 0.00 0.00 %u/%u %u\n", c.running, c.total,
		cur->psid);
}

DEFINE_PROC_FILE(loadavg, fill);
