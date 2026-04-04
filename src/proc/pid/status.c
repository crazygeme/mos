/*
 * status.c — /proc/{pid}/status, /proc/{pid}/stat, /proc/{pid}/statm.
 */
#include "proc_pid.h"
#include <config.h>
#include <macro.h>

/* ── TTY helper ──────────────────────────────────────────────────────── */

/*
 * Look up the controlling TTY device number for a task.
 * Returns the encoded Linux tty_nr ((major<<8)|minor), or 0 if no TTY.
 * Linux virtual consoles are major 4, minor == tty index.
 */
static unsigned get_tty_nr(task_struct *task)
{
	int i;

	if (!task->fds)
		return 0;
	for (i = 0; i < 3; i++) {
		const char *name;
		const char *p;
		int idx;

		if (!task->fds[i].used || !task->fds[i].fp)
			continue;
		name = task->fds[i].fp->f_name;
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

	if (!task->signal)
		return 0;
	for (sig = 1; sig < NSIG; sig++)
		if (task->signal->sig_handlers[sig].sa_handler == SIG_IGN)
			mask |= 1UL << (sig - 1);
	return mask;
}

static unsigned long sig_caught_mask(task_struct *task)
{
	unsigned long mask = 0;
	int sig;

	if (!task->signal)
		return 0;
	for (sig = 1; sig < NSIG; sig++) {
		void (*h)(int) = task->signal->sig_handlers[sig].sa_handler;
		if (h != SIG_DFL && h != SIG_IGN)
			mask |= 1UL << (sig - 1);
	}
	return mask;
}

/* ── /proc/{pid}/status ──────────────────────────────────────────────── */

void fill_status(proc_buf_t *pb, task_struct *task)
{
	const char *cmd =
		task->user->command ? (const char *)task->user->command : "";
	const char *slash = strrchr(cmd, '/');
	const char *name = slash ? slash + 1 : cmd;
	vm_stats_t vm;
	unsigned fdsize = task->fds ? MAX_FD : 0;
	unsigned long sig_pending = task->signal ? task->signal->sig_pending :
						   0;
	unsigned long sig_blocked = task->signal ? task->signal->sig_mask : 0;

	vm_get_stats(task, &vm);

	proc_buf_printf(pb, "Name:      %s\n", name);
	proc_buf_printf(pb, "State:     %c (%s)\n",
			pid_state_char(task->status),
			pid_state_name(task->status));
	proc_buf_printf(pb, "Tgid:      %u\n", task->psid);
	proc_buf_printf(pb, "Pid:       %u\n", task->psid);
	proc_buf_printf(pb, "PPid:      %u\n",
			task->parent ? task->parent->psid : task->psid);
	proc_buf_printf(pb, "TracerPid: 0\n");
	proc_buf_printf(pb, "Uid:       %u\t%u\t%u\t%u\n", task->user->uid,
			task->user->euid, task->user->suid, task->user->euid);
	proc_buf_printf(pb, "Gid:       %u\t%u\t%u\t%u\n", task->user->gid,
			task->user->egid, task->user->sgid, task->user->egid);
	proc_buf_printf(pb, "FDSize:    %u\n", fdsize);
	proc_buf_printf(pb, "Groups:    %u\n", task->user->gid);
	proc_buf_printf(pb, "VmSize:    %u kB\n", vm.total_kb);
	proc_buf_printf(pb, "VmLck:     %u kB\n", 0);
	proc_buf_printf(pb, "VmRSS:     %u kB\n",
			vm.rss_anon_kb + vm.rss_file_kb);
	proc_buf_printf(pb, "VmData:    %u kB\n", vm.data_kb);
	proc_buf_printf(pb, "VmStk:     %u kB\n", vm.stk_kb);
	proc_buf_printf(pb, "VmExe:     %u kB\n", vm.text_kb);
	proc_buf_printf(pb, "VmLib:     %u kB\n", 0);
	proc_buf_printf(pb, "SigPnd: %016lx\n", sig_pending);
	proc_buf_printf(pb, "SigBlk: %016lx\n", sig_blocked);
	proc_buf_printf(pb, "SigIgn: %016lx\n", sig_ignored_mask(task));
	proc_buf_printf(pb, "SigCgt: %016lx\n", sig_caught_mask(task));
	proc_buf_printf(pb, "CapInh: 0000000000000000\n");
	proc_buf_printf(pb, "CapPrm: 0000000000000000\n");
	proc_buf_printf(pb, "CapEff: 0000000000000000\n");
	proc_buf_printf(pb, "voluntary_ctxt_switches:       %u\n",
			task->stats->total_switches -
				task->stats->niv_switches);
	proc_buf_printf(pb, "nonvoluntary_ctxt_switches:    %u\n",
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
void fill_stat(proc_buf_t *pb, task_struct *task)
{
	const char *cmd =
		task->user->command ? (const char *)task->user->command : "";
	const char *slash = strrchr(cmd, '/');
	const char *base = slash ? slash + 1 : cmd;
	char comm[16];
	unsigned tty_nr;
	int tpgid;
	vm_stats_t vm;
	unsigned vsize, rss_pages;
	unsigned stack_start;
	unsigned long utime, stime;

	strncpy(comm, base, 15);
	comm[15] = '\0';

	tty_nr = get_tty_nr(task);
	tpgid = tty_nr ? (int)task->user->group_id : -1;

	vm_get_stats(task, &vm);
	vsize = vm.total_kb * 1024u;
	rss_pages = (vm.rss_anon_kb + vm.rss_file_kb) * 1024u / PAGE_SIZE;
	stack_start = task->user->stack_bottom;

	stime = task->stats->kernel_tickets;
	utime = task_utime(task);

	proc_buf_printf(
		pb,
		"%u (%s) %c %u %u %u %u %d %lu "
		"%lu %lu %lu %lu %lu %lu %ld %ld "
		"%ld %ld %ld %ld %lu %lu %ld "
		"%lu %lu %lu %lu %lu %lu %lu %lu "
		"%lu %lu %lu %lu %d %d\n",
		/* 1  pid         */ task->psid,
		/* 2  comm        */ comm,
		/* 3  state       */ pid_state_char(task->status),
		/* 4  ppid        */ task->parent ? task->parent->psid :
						    task->psid,
		/* 5  pgrp        */ task->user->group_id,
		/* 6  session     */ task->user->session_id,
		/* 7  tty_nr      */ tty_nr,
		/* 8  tpgid       */ tpgid,
		/* 9  flags       */ (unsigned long)0,
		/* 10 minflt      */ (unsigned long)task->stats->pf_minor,
		/* 11 cminflt     */ (unsigned long)0,
		/* 12 majflt      */ (unsigned long)task->stats->pf_major,
		/* 13 cmajflt     */ (unsigned long)0,
		/* 14 utime       */ (unsigned long)utime,
		/* 15 stime       */ (unsigned long)stime,
		/* 16 cutime      */ (long)task->stats->child_utime,
		/* 17 cstime      */ (long)task->stats->child_stime,
		/* 18 priority    */ (long)20,
		/* 19 nice        */ (long)0,
		/* 20 num_threads */ (long)1,
		/* 21 itrealvalue */ (long)0,
		/* 22 starttime   */ (unsigned long)task->stats->start_tickets,
		/* 23 vsize       */ (unsigned long)vsize,
		/* 24 rss         */ (long)rss_pages,
		/* 25 rlim        */ (unsigned long)0x7ffffffful,
		/* 26 startcode   */ (unsigned long)task->user->start_brk,
		/* 27 endcode     */ (unsigned long)task->user->brk,
		/* 28 startstack  */ (unsigned long)stack_start,
		/* 29 kstkesp     */ (unsigned long)0,
		/* 30 kstkeip     */ (unsigned long)0,
		/* 31 signal      */
		(unsigned long)(task->signal ? task->signal->sig_pending : 0),
		/* 32 blocked     */
		(unsigned long)(task->signal ? task->signal->sig_mask : 0),
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
