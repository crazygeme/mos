/*
 * status.c — /proc/{pid}/status, /proc/{pid}/stat, /proc/{pid}/statm.
 */
#include "proc_pid.h"
#include <config.h>
#include <ps/namespace.h>
#include <macro.h>
#include <mm/mmap.h>
#include <ps/usage.h>

/* ── TTY helper ──────────────────────────────────────────────────────── */

/*
 * Look up the controlling TTY device number for a task.
 * Returns the encoded Linux tty_nr ((major<<8)|minor), or 0 if no TTY.
 * Linux virtual consoles are major 4, minor == tty index.
 */
static unsigned get_tty_nr(task_struct *task)
{
	int i;

	if (!task->files)
		return 0;
	for (i = 0; i < 3; i++) {
		const char *name;
		const char *p;
		int idx;

		if (!task->files->fds[i])
			continue;
		name = task->files->fds[i]->f_name;
		if (!name || strncmp(name, "/dev/tty", 8) != 0)
			continue;
		p = name + 8;
		if (*p < '0' || *p > '9')
			continue;
		idx = 0;
		while (*p >= '0' && *p <= '9')
			idx = idx * 10 + (*p++ - '0');
		if (*p != '\0')
			continue;
		return (4u << 8) | (unsigned)idx;
	}
	return 0;
}

/* ── Signal mask helpers ─────────────────────────────────────────────── */

static unsigned long sig_ignored_mask(task_struct *task)
{
	unsigned long mask = 0;
	int sig;

	if (!task->sighand)
		return 0;
	for (sig = 1; sig < NSIG; sig++)
		if (task->sighand->actions[sig].sa_handler == SIG_IGN)
			mask |= 1UL << (sig - 1);
	return mask;
}

static unsigned long sig_caught_mask(task_struct *task)
{
	unsigned long mask = 0;
	int sig;

	if (!task->sighand)
		return 0;
	for (sig = 1; sig < NSIG; sig++) {
		void (*h)(int) = task->sighand->actions[sig].sa_handler;
		if (h != SIG_DFL && h != SIG_IGN)
			mask |= 1UL << (sig - 1);
	}
	return mask;
}

/* ── /proc/{pid}/status ──────────────────────────────────────────────── */

void fill_status(proc_buf_t *pb, task_struct *task)
{
	const char *cmd = task->memory->command ?
				  (const char *)task->memory->command :
				  "";
	const char *slash = strrchr(cmd, '/');
	const char *name = slash ? slash + 1 : cmd;
	vm_stats_t vm;
	unsigned fdsize = task->files ? MAX_FD : 0;
	unsigned long sig_pending = task->sighand ? ps_pending_signals(task) :
						    0;
	unsigned long sig_blocked = task->sighand ? task->signal->sig_mask : 0;

	vm_get_stats(task, &vm);

	proc_buf_printf(pb, "Name:\t%s\n", name);
	proc_buf_printf(pb, "State:\t%c (%s)\n",
			pid_state_char(task->sched->status),
			pid_state_name(task->sched->status));
	proc_buf_printf(pb, "Tgid:\t%u\n",
			ps_pid_visible(task->thread->tgid));
	proc_buf_printf(pb, "Pid:\t%u\n",
			ps_pid_visible(task->life->psid));
	proc_buf_printf(pb, "PPid:\t%u\n",
			ps_pid_visible(task->life->ppid));
	proc_buf_printf(pb, "TracerPid:\t0\n");
	proc_buf_printf(pb, "Uid:\t%u\t%u\t%u\t%u\n",
			task->credentials->uid, task->credentials->euid,
			task->credentials->suid, task->credentials->euid);
	proc_buf_printf(pb, "Gid:\t%u\t%u\t%u\t%u\n",
			task->credentials->gid, task->credentials->egid,
			task->credentials->sgid, task->credentials->egid);
	proc_buf_printf(pb, "FDSize:\t%u\n", fdsize);
	proc_buf_printf(pb, "Threads:\t%u\n",
			proc_thread_count(task->thread->tgid));
	proc_buf_printf(pb, "Groups:\t%u\n", task->credentials->gid);
	proc_buf_printf(pb, "VmSize:\t%llu kB\n",
			(unsigned long long)vm.total_kb);
	proc_buf_printf(pb, "VmLck:\t%u kB\n", 0);
	proc_buf_printf(pb, "VmRSS:\t%llu kB\n",
			(unsigned long long)(vm.rss_anon_kb + vm.rss_file_kb));
	proc_buf_printf(pb, "VmData:\t%llu kB\n",
			(unsigned long long)vm.data_kb);
	proc_buf_printf(pb, "VmStk:\t%llu kB\n",
			(unsigned long long)vm.stk_kb);
	proc_buf_printf(pb, "VmExe:\t%llu kB\n",
			(unsigned long long)vm.text_kb);
	proc_buf_printf(pb, "VmLib:\t%u kB\n", 0);
	proc_buf_printf(pb, "SigPnd:\t%016lx\n", sig_pending);
	proc_buf_printf(pb, "SigBlk:\t%016lx\n", sig_blocked);
	proc_buf_printf(pb, "SigIgn:\t%016lx\n", sig_ignored_mask(task));
	proc_buf_printf(pb, "SigCgt:\t%016lx\n", sig_caught_mask(task));
	task_credentials *u = task->credentials;
	uint64_t bounding = ((uint64_t)u->cap_bounding[1] << 32) |
			    u->cap_bounding[0];
	uint64_t inheritable = ((uint64_t)u->cap_inheritable[1] << 32) |
			       u->cap_inheritable[0];
	uint64_t permitted = u->cap_initialized ?
				     ((uint64_t)u->cap_permitted[1] << 32) |
					     u->cap_permitted[0] :
				     (u->euid == 0 ? bounding : 0);
	uint64_t effective = u->cap_initialized ?
				     ((uint64_t)u->cap_effective[1] << 32) |
					     u->cap_effective[0] :
				     (u->euid == 0 ? bounding : 0);
	proc_buf_printf(pb, "CapInh:\t%016llx\n",
			(unsigned long long)inheritable);
	proc_buf_printf(pb, "CapPrm:\t%016llx\n",
			(unsigned long long)permitted);
	proc_buf_printf(pb, "CapEff:\t%016llx\n",
			(unsigned long long)effective);
	proc_buf_printf(pb, "CapBnd:\t%016llx\n", (unsigned long long)bounding);
	proc_buf_printf(pb, "CapAmb:\t0000000000000000\n");
	proc_buf_printf(pb, "NoNewPrivs:\t%u\n", u->no_new_privs);
	proc_buf_printf(pb, "voluntary_ctxt_switches:\t%u\n",
			task->stats->total_switches -
				task->stats->niv_switches);
	proc_buf_printf(pb, "nonvoluntary_ctxt_switches:\t%u\n",
			task->stats->niv_switches);
}

/* ── /proc/{pid}/stat ────────────────────────────────────────────────── */

/*
 * 39-field Linux 2.4 format:
 *   pid (comm) state ppid pgrp session tty_nr tpgid flags
 *   minflt cminflt majflt cmajflt utime stime cutime cstime
 *   priority nice num_threads itrealvalue starttime vsize rss rlim
 *   startcode endcode startstack kstkesp kstkeip signal blocked
 *   sigignore sigcatch wchan nswap cnswap exit_signal processor
 */
void fill_stat(proc_buf_t *pb, task_struct *task, int group)
{
	const char *cmd = task->memory->command ?
				  (const char *)task->memory->command :
				  "";
	const char *slash = strrchr(cmd, '/');
	const char *base = slash ? slash + 1 : cmd;
	char comm[16];
	unsigned tty_nr;
	int tpgid;
	vm_stats_t vm;
	uint64_t vsize, rss_pages;
	vaddr_t stack_start;
	unsigned long long utime, stime;

	strncpy(comm, base, 15);
	comm[15] = '\0';

	tty_nr = get_tty_nr(task);
	tpgid = tty_nr ? (int)task->thread->group_id : -1;

	vm_get_stats(task, &vm);
	vsize = vm.total_kb * 1024;
	rss_pages = (vm.rss_anon_kb + vm.rss_file_kb) / (PAGE_SIZE / 1024);
	stack_start = vm.start_stack;

	stime = ps_usage_read(group ? &task->thread->kernel_tickets :
				      &task->stats->kernel_tickets);
	utime = group ? ps_usage_read(&task->thread->user_tickets) :
			task_utime(task);

	proc_buf_printf(
		pb,
		"%u (%s) %c %u %u %u %u %d %lu "
		"%lu %lu %lu %lu %llu %llu %llu %llu "
		"%ld %ld %ld %ld %lu %llu %lld "
		"%lu %lu %lu %lu %lu %lu %lu %lu "
		"%lu %lu %lu %lu %d %d\n",
		/* 1  pid         */ ps_pid_visible(task->life->psid),
		/* 2  comm        */ comm,
		/* 3  state       */ pid_state_char(task->sched->status),
		/* 4  ppid        */ ps_pid_visible(task->life->ppid),
		/* 5  pgrp        */ ps_pid_visible(task->thread->group_id),
		/* 6  session     */ ps_pid_visible(task->thread->session_id),
		/* 7  tty_nr      */ tty_nr,
		/* 8  tpgid       */ tpgid,
		/* 9  flags       */ (unsigned long)0,
		/* 10 minflt      */ (unsigned long)task->stats->pf_minor,
		/* 11 cminflt     */ (unsigned long)0,
		/* 12 majflt      */ (unsigned long)task->stats->pf_major,
		/* 13 cmajflt     */ (unsigned long)0,
		/* 14 utime       */ utime,
		/* 15 stime       */ stime,
		/* 16 cutime      */ ps_usage_read(&task->thread->child_utime),
		/* 17 cstime      */ ps_usage_read(&task->thread->child_stime),
		/* 18 priority    */ (long)20,
		/* 19 nice        */ (long)0,
		/* 20 num_threads */ (long)proc_thread_count(task->thread->tgid),
		/* 21 itrealvalue */ (long)0,
		/* 22 starttime   */ (unsigned long)task->stats->start_tickets,
		/* 23 vsize       */ (unsigned long long)vsize,
		/* 24 rss         */ (long long)rss_pages,
		/* 25 rlim        */ (unsigned long)0x7ffffffful,
		/* 26 startcode   */ (unsigned long)vm.start_brk,
		/* 27 endcode     */ (unsigned long)vm.brk,
		/* 28 startstack  */ (unsigned long)stack_start,
		/* 29 kstkesp     */ (unsigned long)0,
		/* 30 kstkeip     */ (unsigned long)0,
		/* 31 signal      */
		(unsigned long)(task->sighand ? ps_pending_signals(task) : 0),
		/* 32 blocked     */
		(unsigned long)(task->sighand ? task->signal->sig_mask : 0),
		/* 33 sigignore   */ sig_ignored_mask(task),
		/* 34 sigcatch    */ sig_caught_mask(task),
		/* 35 wchan       */ (unsigned long)0,
		/* 36 nswap       */ (unsigned long)0,
		/* 37 cnswap      */ (unsigned long)0,
		/* 38 exit_signal */ SIGCHLD,
		/* 39 processor   */ 0);
}

/* ── /proc/{pid}/statm ───────────────────────────────────────────────── */

void fill_statm(proc_buf_t *pb, task_struct *task)
{
	vm_fill_statm(pb, task);
}
