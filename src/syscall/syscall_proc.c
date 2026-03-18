/*
 * syscall_proc.c — process management syscall handlers.
 *
 * Covers: getpid/ppid/pgrp, uid/gid, setsid, wait4, kill, brk,
 *         sched_yield, alarm, pause, signals, rlimit, personality.
 */

#include <ps/ps.h>
#include <fs/fs.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include "syscall_internal.h"

/* personality flags */
enum {
	UNAME26 = 0x0020000,
	ADDR_NO_RANDOMIZE = 0x0040000,
	FDPIC_FUNCPTRS = 0x0080000,
	MMAP_PAGE_ZERO = 0x0100000,
	ADDR_COMPAT_LAYOUT = 0x0200000,
	READ_IMPLIES_EXEC = 0x0400000,
	ADDR_LIMIT_32BIT = 0x0800000,
	SHORT_INODE = 0x1000000,
	WHOLE_SECONDS = 0x2000000,
	STICKY_TIMEOUTS = 0x4000000,
	ADDR_LIMIT_3GB = 0x8000000,
};

enum {
	PER_LINUX = 0x0000,
	PER_LINUX_32BIT = 0x0000 | ADDR_LIMIT_32BIT,
	PER_LINUX_FDPIC = 0x0000 | FDPIC_FUNCPTRS,
	PER_SVR4 = 0x0001 | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
	PER_SVR3 = 0x0002 | STICKY_TIMEOUTS | SHORT_INODE,
	PER_SCOSVR3 = 0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS | SHORT_INODE,
	PER_OSR5 = 0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS,
	PER_WYSEV386 = 0x0004 | STICKY_TIMEOUTS | SHORT_INODE,
	PER_ISCR4 = 0x0005 | STICKY_TIMEOUTS,
	PER_BSD = 0x0006,
	PER_SUNOS = 0x0006 | STICKY_TIMEOUTS,
	PER_XENIX = 0x0007 | STICKY_TIMEOUTS | SHORT_INODE,
	PER_LINUX32 = 0x0008,
	PER_LINUX32_3GB = 0x0008 | ADDR_LIMIT_3GB,
	PER_IRIX32 = 0x0009 | STICKY_TIMEOUTS,
	PER_IRIXN32 = 0x000a | STICKY_TIMEOUTS,
	PER_IRIX64 = 0x000b | STICKY_TIMEOUTS,
	PER_RISCOS = 0x000c,
	PER_SOLARIS = 0x000d | STICKY_TIMEOUTS,
	PER_UW7 = 0x000e | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
	PER_OSF4 = 0x000f,
	PER_HPUX = 0x0010,
	PER_MASK = 0x00ff,
};

int sys_getpid()
{
	task_struct *cur = CURRENT_TASK();

	if (TestControl.verbos)
		klog("getpid() = %d\n", cur->psid);

	return cur->psid;
}

int sys_getppid()
{
	task_struct *cur = CURRENT_TASK();

	if (TestControl.verbos)
		klog("getppid\n");

	return cur->parent->psid;
}

int sys_getpgrp(unsigned pid)
{
	if (TestControl.verbos)
		klog("getpgrp\n");

	return 0; /* FIXME */
}

int sys_setpgid(unsigned pid, unsigned pgid)
{
	if (TestControl.verbos)
		klog("setpgid\n");

	if (pid == 0) {
		task_struct *cur = CURRENT_TASK();
		cur->user->group_id = pgid;
	}
	return 0; /* FIXME */
}

int sys_getuid()
{
	if (TestControl.verbos)
		klog("getuid\n");

	return 0;
}

int sys_getgid()
{
	if (TestControl.verbos)
		klog("getgid\n");

	return 0;
}

int sys_geteuid()
{
	if (TestControl.verbos)
		klog("geteuid\n");

	return 0;
}

int sys_getegid()
{
	if (TestControl.verbos)
		klog("getegid\n");

	return 0;
}

int sys_setreuid(unsigned ruid, unsigned euid)
{
	if (TestControl.verbos)
		klog("setreuid(%d, %d)\n", ruid, euid);

	return 0;
}

int sys_setregid(unsigned pid, unsigned pgid)
{
	if (TestControl.verbos)
		klog("setregid(%d, %d)\n", pid, pgid);

	return 0;
}

int sys_setsid()
{
	task_struct *cur = CURRENT_TASK();

	if (TestControl.verbos)
		klog("setsid() = %d\n", cur->psid);

	cur->user->session_id = cur->psid;
	cur->user->group_id = cur->psid;
	return cur->psid;
}

int sys_wait4(int pid, int *status, int options, void *rusage)
{
	if (pid == -1)
		return do_waitpid(0, status, options, rusage);
	else if (pid < -1)
		return 0; /* FIXME: wait on group id */
	else
		return do_waitpid(pid, status, options, rusage);
}

int sys_kill(unsigned pid, int sig)
{
	if (TestControl.verbos)
		klog("kill(%d, %d)\n", pid, sig);

	return -1;
}

int sys_brk(unsigned _top)
{
	task_struct *task = CURRENT_TASK();
	unsigned size, pages, top, ret;

	top = _top;
	if (task->user->heap_top == USER_HEAP_BEGIN) {
		do_mmap(task->user->heap_top, PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, -1, 0);
		task->user->heap_top += PAGE_SIZE;
	}

	if (top == 0) {
		ret = task->user->heap_top;
	} else if (top >= USER_HEAP_END) {
		ret = task->user->heap_top;
	} else if (top > task->user->heap_top) {
		size = top - task->user->heap_top;
		pages = (size - 1) / PAGE_SIZE + 1;
		do_mmap(task->user->heap_top, PAGE_SIZE * pages,
			PROT_READ | PROT_WRITE, 0, -1, 0);
		top = task->user->heap_top + pages * PAGE_SIZE;
		task->user->heap_top = top;
		ret = top;
	} else {
		if (top < USER_HEAP_BEGIN)
			top = USER_HEAP_BEGIN;
		size = task->user->heap_top - top;
		pages = (size - 1) / PAGE_SIZE + 1;
		do_munmap(top & PAGE_SIZE_MASK, pages * PAGE_SIZE);
		task->user->heap_top = top;
		ret = top;
	}

	if (TestControl.verbos)
		klog("brk(%x) = %x\n", _top, ret);

	return ret;
}

int sys_sched_yield()
{
	if (TestControl.verbos)
		klog("yield\n");

	task_sched();
	return 0;
}

unsigned sys_alarm(unsigned seconds)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long long now = time_now_ms();
	unsigned remaining = 0;

	if (cur->alarm_expire_ms > now)
		remaining =
			(unsigned)((cur->alarm_expire_ms - now + 999) / 1000);

	cur->alarm_expire_ms =
		seconds ? (now + (unsigned long long)seconds * 1000) : 0;

	if (TestControl.verbos)
		klog("alarm(%u) = %u\n", seconds, remaining);

	return remaining;
}

int sys_pause()
{
	if (TestControl.verbos)
		klog("pause\n");

	PAUSE();
	return 0;
}

int sys_sigaction(int sig, void *act, void *oact)
{
	return -1; /* FIXME: no signal support */
}

int sys_sigprocmask(int how, void *set, void *oset)
{
	return -1; /* FIXME: no signal support */
}

int sys_getrlimit(int resource, void *limit)
{
	if (TestControl.verbos)
		klog("getrlimit\n");

	return -1;
}

long sys_personality(unsigned int personality)
{
	if (TestControl.verbos)
		klog("personality\n");

	return PER_LINUX32_3GB;
}
