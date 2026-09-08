/*
 * /proc/stat — system-wide CPU and process statistics.
 *
 * Format (Linux-compatible):
 *
 *   cpu  <user> <nice> <system> <idle> <iowait> <irq> <softirq> <steal> ...
 *   cpu0 <user> <nice> <system> <idle> ...
 *   ...
 *   intr <total>
 *   ctxt <context_switches>
 *   btime <boot_epoch>
 *   processes <forks_since_boot>
 *   procs_running <N>
 *   procs_blocked <N>
 *   softirq <total> ...
 *
 * Time units are USER_HZ (= 100 ticks/second).  Our task->user_tickets and
 * task->kernel_tickets are already in 10 ms units (100/s), so they map 1:1.
 *
 * Per-CPU breakdown: the scheduler currently keeps aggregate task accounting
 * rather than separate user/system counters for each CPU.  Split the
 * aggregate evenly across online CPUs so consumers such as procps/top see a
 * valid, advancing sample for every CPU instead of treating secondary CPUs as
 * absent.
 */

#include "hw/time.h"
#include <ps/ps.h>
#include <ps/smp.h>
#include "common.h"

/* Externs from ps_sched.c */
extern unsigned task_schedule_count;

typedef struct {
	unsigned user, system;
	unsigned procs_running, procs_blocked, processes;
} stat_ctx_t;

static void stat_collect(task_struct *task, void *ctx)
{
	stat_ctx_t *c = (stat_ctx_t *)ctx;
	if (task->psid == 0xffffffff)
		return;

	if (task->type == ps_kernel)
		return;

	c->processes++;

	if (task->priority != ps_idle) {
		c->user += task_utime(task);
		c->system += task->stats->kernel_tickets;
	}

	if (task->status == ps_running || task->status == ps_ready)
		c->procs_running++;
	else if (task->status == ps_waiting)
		c->procs_blocked++;
}

static void fill(proc_buf_t *pb)
{
	int i, ncpu;
	stat_ctx_t c = { 0, 0, 0, 0, 0 };
	unsigned wall, idle;

	ps_enum_all(stat_collect, &c);

	/* Idle = wall-clock jiffies since boot minus all busy (user+system) time. */
	wall = (unsigned)time_now_tickets() * smp_cpu_count();
	idle = (wall > c.user + c.system) ? wall - c.user - c.system : 0;

	ncpu = smp_cpu_count();

	/* ---- aggregate cpu line ---- */
	proc_buf_printf(pb, "cpu  %u 0 %u %u 0 0 0 0 0 0\n", c.user, c.system,
			idle);

	/* ---- per-CPU lines ----
	 * Keep each field's sum close to the aggregate.  Remainders are assigned
	 * to the first CPUs, matching integer-jiffy accounting semantics. */
	unsigned user_each = c.user / (unsigned)ncpu;
	unsigned user_rem = c.user % (unsigned)ncpu;
	unsigned system_each = c.system / (unsigned)ncpu;
	unsigned system_rem = c.system % (unsigned)ncpu;
	unsigned idle_each = idle / (unsigned)ncpu;
	unsigned idle_rem = idle % (unsigned)ncpu;
	for (i = 0; i < ncpu; i++) {
		proc_buf_printf(pb, "cpu%d %u 0 %u %u 0 0 0 0 0 0\n", i,
				user_each + (i < (int)user_rem),
				system_each + (i < (int)system_rem),
				idle_each + (i < (int)idle_rem));
	}

	proc_buf_printf(pb, "intr 0\n");
	proc_buf_printf(pb, "ctxt %u\n", task_schedule_count);
	proc_buf_printf(pb, "btime 0\n");
	proc_buf_printf(pb, "processes %u\n", c.processes);
	proc_buf_printf(pb, "procs_running %u\n", c.procs_running);
	proc_buf_printf(pb, "procs_blocked %u\n", c.procs_blocked);
	proc_buf_printf(pb, "softirq 0 0 0 0 0 0 0 0 0 0 0\n");
}

DEFINE_PROC_FILE(stat, fill);
