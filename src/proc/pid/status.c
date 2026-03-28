/*
 * status.c — /proc/{pid}/status, /proc/{pid}/stat, /proc/{pid}/statm.
 */
#include "hw/time.h"
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

/* ── /proc/{pid}/status ──────────────────────────────────────────────── */

void fill_status(proc_buf_t *pb, task_struct *task)
{
	const char *cmd =
		task->user->command ? (const char *)task->user->command : "";
	const char *slash = strrchr(cmd, '/');
	const char *name = slash ? slash + 1 : cmd;
	vm_stats_t vm;
	unsigned fdsize = task->fds ? 256 : 0;

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
	proc_buf_printf(pb, "Uid:       0\t0\t0\t0\n");
	proc_buf_printf(pb, "Gid:       0\t0\t0\t0\n");
	proc_buf_printf(pb, "FDSize:    %u\n", fdsize);
	proc_buf_printf(pb, "Groups:    0 1 2 3 4 6 10\n");
	proc_buf_printf(pb, "VmSize:    %u kB\n", vm.total_kb);
	proc_buf_printf(pb, "VmLck:     %u kB\n", 0);
	proc_buf_printf(pb, "VmRSS:     %u kB\n",
			vm.rss_anon_kb + vm.rss_file_kb);
	proc_buf_printf(pb, "VmData:    %u kB\n", vm.data_kb);
	proc_buf_printf(pb, "VmStk:     %u kB\n", vm.stk_kb);
	proc_buf_printf(pb, "VmExe:     %u kB\n", vm.text_kb);
	proc_buf_printf(pb, "VmLib:     %u kB\n", 0);
	proc_buf_printf(pb, "SigPnd: 0000000000000000\n");
	proc_buf_printf(pb, "SigBlk: 0000000000000000\n");
	proc_buf_printf(pb, "SigIgn: 0000000000000000\n");
	proc_buf_printf(pb, "SigCgt: 0000000000000000\n");
	proc_buf_printf(pb, "CapInh: 0000000000000000\n");
	proc_buf_printf(pb, "CapPrm: 0000000000000000\n");
	proc_buf_printf(pb, "CapEff: 0000000000000000\n");
	proc_buf_printf(pb, "voluntary_ctxt_switches:       %u\n",
			task->total_switches - task->niv_switches);
	proc_buf_printf(pb, "nonvoluntary_ctxt_switches:    %u\n",
			task->niv_switches);
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

	strncpy(comm, base, 15);
	comm[15] = '\0';

	tty_nr = get_tty_nr(task);
	tpgid = tty_nr ? (int)task->user->group_id : -1;

	vm_get_stats(task, &vm);
	vsize = vm.total_kb * 1024u;
	rss_pages = (vm.rss_anon_kb + vm.rss_file_kb) * 1024u / PAGE_SIZE;
	stack_start = task->user->stack_bottom;

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
		/* 10 minflt      */ (unsigned long)task->pf_minor,
		/* 11 cminflt     */ (unsigned long)0,
		/* 12 majflt      */ (unsigned long)task->pf_major,
		/* 13 cmajflt     */ (unsigned long)0,
		/* 14 utime       */
		(unsigned long)(time_now_tickets() - task->start_tickets -
				task->kernel_tickets - task->idle_tickets),
		/* 15 stime       */ (unsigned long)task->kernel_tickets,
		/* 16 cutime      */ (long)0,
		/* 17 cstime      */ (long)0,
		/* 18 priority    */ (long)20,
		/* 19 nice        */ (long)0,
		/* 20 num_threads */ (long)0,
		/* 21 itrealvalue */ (long)0,
		/* 22 starttime   */ (unsigned long)0,
		/* 23 vsize       */ (unsigned long)vsize,
		/* 24 rss         */ (long)rss_pages,
		/* 25 rlim        */ (unsigned long)0x7ffffffful,
		/* 26 startcode   */ (unsigned long)0,
		/* 27 endcode     */ (unsigned long)0,
		/* 28 startstack  */ (unsigned long)stack_start,
		/* 29 kstkesp     */ (unsigned long)0,
		/* 30 kstkeip     */ (unsigned long)0,
		/* 31 signal      */ (unsigned long)0,
		/* 32 blocked     */ (unsigned long)0,
		/* 33 sigignore   */ (unsigned long)0,
		/* 34 sigcatch    */ (unsigned long)0,
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
