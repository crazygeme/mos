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
 * CPU counters are cumulative per-CPU timer samples in USER_HZ units.
 * They include kernel tasks and retain usage after tasks are reaped.
 */

#include "device/time.h"
#include <ps/ps.h>
#include <ps/smp.h>
#include <ps/usage.h>
#include "common.h"

/* Externs from ps_sched.c */
extern unsigned task_schedule_count;

typedef struct {
	unsigned procs_running, procs_blocked, processes;
} stat_ctx_t;

static void stat_collect(task_struct *task, void *ctx)
{
	stat_ctx_t *c = (stat_ctx_t *)ctx;
	if (task->life->psid == 0xffffffff)
		return;

	if (task->life->type == ps_kernel)
		return;

	c->processes++;

	if (task->sched->status == ps_running ||
	    task->sched->status == ps_ready)
		c->procs_running++;
	else if (task->sched->status == ps_waiting)
		c->procs_blocked++;
}

static void fill(proc_buf_t *pb)
{
	int i, ncpu;
	stat_ctx_t c = { 0, 0, 0 };
	cpu_usage_t samples[SMP_MAX_CPUS];
	cpu_usage_t total = { 0, 0, 0 };

	ps_enum_all(stat_collect, &c);
	ncpu = smp_cpu_count();
	for (i = 0; i < ncpu; i++) {
		ps_cpu_usage(i, &samples[i]);
		total.user += samples[i].user;
		total.system += samples[i].system;
		total.idle += samples[i].idle;
	}
	proc_buf_printf(pb, "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n", total.user,
			total.system, total.idle);
	for (i = 0; i < ncpu; i++)
		proc_buf_printf(pb, "cpu%d %llu 0 %llu %llu 0 0 0 0 0 0\n", i,
				samples[i].user, samples[i].system,
				samples[i].idle);

	proc_buf_printf(pb, "intr 0\n");
	proc_buf_printf(pb, "ctxt %u\n", task_schedule_count);
	proc_buf_printf(pb, "btime 0\n");
	proc_buf_printf(pb, "processes %u\n", c.processes);
	proc_buf_printf(pb, "procs_running %u\n", c.procs_running);
	proc_buf_printf(pb, "procs_blocked %u\n", c.procs_blocked);
	proc_buf_printf(pb, "softirq 0 0 0 0 0 0 0 0 0 0 0\n");
}

DEFINE_PROC_FILE(stat, fill);
