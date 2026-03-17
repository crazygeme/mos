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

/* ---- Callback accumulator (module-static, written single-threadedly) ---- */
static unsigned g_user;
static unsigned g_system;
static unsigned g_idle;
static unsigned g_procs_running;
static unsigned g_procs_blocked;
static unsigned g_processes;

static void stat_collect(task_struct *task)
{
	if (task->psid == 0xffffffff)
		return;

	g_processes++;

	if (task->priority == ps_idle) {
		/* idle task: its CPU time counts as idle */
		g_idle += task->kernel_tickets + task->user_tickets;
	} else {
		g_user += task->user_tickets;
		g_system += task->kernel_tickets;
	}

	if (task->status == ps_running || task->status == ps_ready)
		g_procs_running++;
	else if (task->status == ps_waiting)
		g_procs_blocked++;
}

static void fill(void *buf, size_t size)
{
	char *p = buf;
	int i, ncpu;

	memset(buf, 0, size);

	g_user = g_system = g_idle = 0;
	g_procs_running = g_procs_blocked = g_processes = 0;
	ps_enum_all(stat_collect);

	ncpu = ncpus > 0 ? ncpus : 1;

	/* ---- aggregate cpu line ---- */
	sprintf(p, "cpu  %u 0 %u %u 0 0 0 0 0 0\n", g_user, g_system, g_idle);
	p += strlen(p);

	/* ---- per-CPU lines ---- */
	for (i = 0; i < ncpu; i++) {
		if (i == 0)
			/* cpu0 carries the full aggregate */
			sprintf(p, "cpu%d %u 0 %u %u 0 0 0 0 0 0\n", i, g_user,
				g_system, g_idle);
		else
			sprintf(p, "cpu%d 0 0 0 0 0 0 0 0 0 0\n", i);
		p += strlen(p);
	}

	sprintf(p, "intr 0\n");
	p += strlen(p);
	sprintf(p, "ctxt %u\n", task_schedule_count);
	p += strlen(p);
	sprintf(p, "btime 0\n");
	p += strlen(p);
	sprintf(p, "processes %u\n", g_processes);
	p += strlen(p);
	sprintf(p, "procs_running %u\n", g_procs_running);
	p += strlen(p);
	sprintf(p, "procs_blocked %u\n", g_procs_blocked);
	p += strlen(p);
	sprintf(p, "softirq 0 0 0 0 0 0 0 0 0 0 0\n");
	p += strlen(p);
}

DEFINE_PROC_FILE(stat, fill);
