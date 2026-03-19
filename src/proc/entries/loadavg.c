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

static unsigned g_total;
static unsigned g_running;

static void count_task(task_struct *task)
{
	g_total++;
	if (task->status == ps_running)
		g_running++;
}

static void fill(void *buf, size_t size)
{
	task_struct *cur = CURRENT_TASK();

	g_total = g_running = 0;
	ps_enum_all(count_task);

	memset(buf, 0, size);
	sprintf(buf, "0.00 0.00 0.00 %u/%u %u\n",
		g_running, g_total, cur->psid);
}

DEFINE_PROC_FILE(loadavg, fill);
