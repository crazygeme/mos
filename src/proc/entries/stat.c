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
 * Per-CPU breakdown: we do not track per-CPU time, so cpu0 carries the full
 * aggregate and cpu1..N are reported as all-zero.
 */

#include <ps/ps.h>
#include <hw/cpu.h>
#include "generic.h"

/* Externs from ps_sched.c */
extern unsigned task_schedule_count;

typedef struct {
	unsigned user, system, idle;
	unsigned procs_running, procs_blocked, processes;
} stat_ctx_t;

static void stat_collect(task_struct *task, void *ctx)
{
	stat_ctx_t *c = (stat_ctx_t *)ctx;
	if (task->psid == 0xffffffff)
		return;

	c->processes++;

	if (task->priority == ps_idle) {
		c->idle += task->kernel_tickets + task->user_tickets;
	} else {
		c->user += task->user_tickets;
		c->system += task->kernel_tickets;
	}

	if (task->status == ps_running || task->status == ps_ready)
		c->procs_running++;
	else if (task->status == ps_waiting)
		c->procs_blocked++;
}

static void fill(void *buf, size_t size)
{
	char *p = buf;
	int i, ncpu;
	stat_ctx_t c = { 0, 0, 0, 0, 0, 0 };

	memset(buf, 0, size);

	ps_enum_all(stat_collect, &c);

	ncpu = ncpus > 0 ? ncpus : 1;

	/* ---- aggregate cpu line ---- */
	p += sprintf(p, "cpu  %u 0 %u %u 0 0 0 0 0 0\n", c.user, c.system,
		     c.idle);

	/* ---- per-CPU lines ---- */
	for (i = 0; i < ncpu; i++) {
		if (i == 0)
			/* cpu0 carries the full aggregate */
			p += sprintf(p, "cpu%d %u 0 %u %u 0 0 0 0 0 0\n", i,
				     c.user, c.system, c.idle);
		else
			p += sprintf(p, "cpu%d 0 0 0 0 0 0 0 0 0 0\n", i);
	}

	p += sprintf(p, "intr 0\n");
	p += sprintf(p, "ctxt %u\n", task_schedule_count);
	p += sprintf(p, "btime 0\n");
	p += sprintf(p, "processes %u\n", c.processes);
	p += sprintf(p, "procs_running %u\n", c.procs_running);
	p += sprintf(p, "procs_blocked %u\n", c.procs_blocked);
	p += sprintf(p, "softirq 0 0 0 0 0 0 0 0 0 0 0\n");
}

DEFINE_PROC_FILE(stat, fill);
