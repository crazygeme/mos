/*
 * syscall_proc.c — process management syscall handlers.
 *
 * Covers: getpid/ppid/pgrp, uid/gid, setsid, wait4, brk,
 *         sched_yield, alarm, rlimit, personality.
 *
 * Signal-related syscalls live in ps/ps_signal.c.
 */
#include <ps/ps.h>
#include <fs/fs.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/vdso.h>
#include <hw/cpu.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <errno.h>
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

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getpid() = %d\n", cur->tgid);

	return cur->tgid;
}

int sys_getppid()
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getppid() = %d\n", cur->ppid);

	return cur->ppid;
}

int sys_getpgrp(unsigned pid)
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getpgrp() = %d\n", cur->user->group_id);

	return cur->user->group_id;
}

int sys_getpgid(unsigned pid)
{
	task_struct *t;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getpgid(%d)\n", pid);

	if (pid == 0)
		return CURRENT_TASK()->user->group_id;

	t = ps_find_process(pid);
	if (!t)
		return -ESRCH;
	return t->user->group_id;
}

int sys_setpgid(unsigned pid, unsigned pgid)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *t;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setpgid(%d, %d)\n", pid, pgid);

	if (pid == 0)
		t = cur;
	else {
		t = ps_find_process(pid);
		if (!t)
			return -ESRCH;
	}

	/* pgid 0 means use the target process's own pid as pgid */
	if (pgid == 0)
		pgid = t->psid;

	t->user->group_id = pgid;
	return 0;
}

int sys_getsid(unsigned pid)
{
	task_struct *t;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getsid(%d)\n", pid);

	if (pid == 0)
		return CURRENT_TASK()->user->session_id;

	t = ps_find_process(pid);
	if (!t)
		return -ESRCH;
	return t->user->session_id;
}

int sys_setsid()
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setsid() = %d\n", cur->psid);

	cur->user->session_id = cur->psid;
	cur->user->group_id = cur->psid;
	return cur->psid;
}

int sys_getuid()
{
	task_struct *cur = CURRENT_TASK();
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getuid() = %d\n", cur->user->uid);

	return cur->user->uid;
}

int sys_getgid()
{
	task_struct *cur = CURRENT_TASK();
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getgid() = %d\n", cur->user->gid);

	return cur->user->gid;
}

int sys_geteuid()
{
	task_struct *cur = CURRENT_TASK();
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("geteuid() = %d\n", cur->user->euid);

	return cur->user->euid;
}

int sys_getegid()
{
	task_struct *cur = CURRENT_TASK();
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getegid() = %d\n", cur->user->egid);

	return cur->user->egid;
}

/*
 * setuid — Linux semantics:
 *   euid==0: sets ruid=euid=suid=uid (privilege drop)
 *   euid!=0: can only set euid to current ruid or suid
 */
int sys_setuid(unsigned uid)
{
	task_struct *cur = CURRENT_TASK();
	user_enviroment *u = cur->user;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setuid(%d)\n", uid);

	if (u->euid == 0) {
		u->uid = uid;
		u->euid = uid;
		u->suid = uid;
		u->fsuid = uid;
	} else if (uid == u->uid || uid == u->suid) {
		u->euid = uid;
		u->fsuid = uid;
	} else {
		return -EPERM;
	}
	return 0;
}

/*
 * setgid — Linux semantics:
 *   euid==0: sets rgid=egid=sgid=gid
 *   euid!=0: can only set egid to current rgid or sgid
 */
int sys_setgid(unsigned gid)
{
	task_struct *cur = CURRENT_TASK();
	user_enviroment *u = cur->user;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setgid(%d)\n", gid);

	if (u->euid == 0) {
		u->gid = gid;
		u->egid = gid;
		u->sgid = gid;
		u->fsgid = gid;
	} else if (gid == u->gid || gid == u->sgid) {
		u->egid = gid;
		u->fsgid = gid;
	} else {
		return -EPERM;
	}
	return 0;
}

/*
 * setreuid — set real and/or effective uid.
 * -1 means "leave unchanged".
 */
int sys_setreuid(unsigned ruid, unsigned euid)
{
	task_struct *cur = CURRENT_TASK();
	user_enviroment *u = cur->user;
	unsigned new_ruid = (ruid == (unsigned)-1) ? u->uid : ruid;
	unsigned new_euid = (euid == (unsigned)-1) ? u->euid : euid;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setreuid(%d, %d)\n", ruid, euid);

	if (u->euid != 0) {
		/* unprivileged: ruid must be current ruid or euid */
		if (ruid != (unsigned)-1 && ruid != u->uid && ruid != u->euid)
			return -EPERM;
		/* euid must be current ruid, euid, or suid */
		if (euid != (unsigned)-1 && euid != u->uid && euid != u->euid &&
		    euid != u->suid)
			return -EPERM;
	}

	/* If euid changes, update suid to new euid */
	if (euid != (unsigned)-1 && new_euid != u->euid)
		u->suid = new_euid;

	u->uid = new_ruid;
	u->euid = new_euid;
	u->fsuid = new_euid;
	return 0;
}

/*
 * setregid — set real and/or effective gid.
 * -1 means "leave unchanged".
 */
int sys_setregid(unsigned rgid, unsigned egid)
{
	task_struct *cur = CURRENT_TASK();
	user_enviroment *u = cur->user;
	unsigned new_rgid = (rgid == (unsigned)-1) ? u->gid : rgid;
	unsigned new_egid = (egid == (unsigned)-1) ? u->egid : egid;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setregid(%d, %d)\n", rgid, egid);

	if (u->euid != 0) {
		if (rgid != (unsigned)-1 && rgid != u->gid && rgid != u->egid)
			return -EPERM;
		if (egid != (unsigned)-1 && egid != u->gid && egid != u->egid &&
		    egid != u->sgid)
			return -EPERM;
	}

	if (egid != (unsigned)-1 && new_egid != u->egid)
		u->sgid = new_egid;

	u->gid = new_rgid;
	u->egid = new_egid;
	u->fsgid = new_egid;
	return 0;
}

/*
 * setresuid — set real, effective, and saved-set uid.
 * -1 means "leave unchanged".
 */
int sys_setresuid(unsigned ruid, unsigned euid, unsigned suid)
{
	task_struct *cur = CURRENT_TASK();
	user_enviroment *u = cur->user;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setresuid(%d, %d, %d)\n", ruid, euid, suid);

	if (u->euid != 0) {
		/* unprivileged: each value must be current ruid, euid, or suid */
		if (ruid != (unsigned)-1 && ruid != u->uid && ruid != u->euid &&
		    ruid != u->suid)
			return -EPERM;
		if (euid != (unsigned)-1 && euid != u->uid && euid != u->euid &&
		    euid != u->suid)
			return -EPERM;
		if (suid != (unsigned)-1 && suid != u->uid && suid != u->euid &&
		    suid != u->suid)
			return -EPERM;
	}

	if (ruid != (unsigned)-1)
		u->uid = ruid;
	if (euid != (unsigned)-1) {
		u->euid = euid;
		u->fsuid = euid;
	}
	if (suid != (unsigned)-1)
		u->suid = suid;
	return 0;
}

int sys_getresuid(unsigned *ruid, unsigned *euid, unsigned *suid)
{
	user_enviroment *u = CURRENT_TASK()->user;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getresuid\n");

	if (ruid)
		*ruid = u->uid;
	if (euid)
		*euid = u->euid;
	if (suid)
		*suid = u->suid;
	return 0;
}

/*
 * setresgid — set real, effective, and saved-set gid.
 * -1 means "leave unchanged".
 */
int sys_setresgid(unsigned rgid, unsigned egid, unsigned sgid)
{
	task_struct *cur = CURRENT_TASK();
	user_enviroment *u = cur->user;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setresgid(%d, %d, %d)\n", rgid, egid, sgid);

	if (u->euid != 0) {
		if (rgid != (unsigned)-1 && rgid != u->gid && rgid != u->egid &&
		    rgid != u->sgid)
			return -EPERM;
		if (egid != (unsigned)-1 && egid != u->gid && egid != u->egid &&
		    egid != u->sgid)
			return -EPERM;
		if (sgid != (unsigned)-1 && sgid != u->gid && sgid != u->egid &&
		    sgid != u->sgid)
			return -EPERM;
	}

	if (rgid != (unsigned)-1)
		u->gid = rgid;
	if (egid != (unsigned)-1) {
		u->egid = egid;
		u->fsgid = egid;
	}
	if (sgid != (unsigned)-1)
		u->sgid = sgid;
	return 0;
}

int sys_getresgid(unsigned *rgid, unsigned *egid, unsigned *sgid)
{
	user_enviroment *u = CURRENT_TASK()->user;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getresgid\n");

	if (rgid)
		*rgid = u->gid;
	if (egid)
		*egid = u->egid;
	if (sgid)
		*sgid = u->sgid;
	return 0;
}

/*
 * setfsuid/setfsgid — set filesystem uid/gid.
 * Returns the previous value (always succeeds unless out-of-range).
 */
int sys_setfsuid(unsigned fsuid)
{
	user_enviroment *u = CURRENT_TASK()->user;
	unsigned old = u->fsuid;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setfsuid(%d)\n", fsuid);

	/* Only root or matching uid/euid/suid can change fsuid */
	if (u->euid == 0 || fsuid == u->uid || fsuid == u->euid ||
	    fsuid == u->suid || fsuid == u->fsuid)
		u->fsuid = fsuid;

	return old;
}

int sys_setfsgid(unsigned fsgid)
{
	user_enviroment *u = CURRENT_TASK()->user;
	unsigned old = u->fsgid;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setfsgid(%d)\n", fsgid);

	if (u->euid == 0 || fsgid == u->gid || fsgid == u->egid ||
	    fsgid == u->sgid || fsgid == u->fsgid)
		u->fsgid = fsgid;

	return old;
}

int sys_wait4(int pid, int *status, int options, void *rusage)
{
	if (pid == -1)
		return do_waitpid(0, status, options, rusage);
	else if (pid < -1)
		return do_waitpid_pgrp((unsigned)(-pid), status, options,
				       rusage);
	else
		return do_waitpid(pid, status, options, rusage);
}

int sys_brk(unsigned _top)
{
	task_struct *task = CURRENT_TASK();
	heap_state *heap = task->user->heap;
	unsigned top, ret;
	unsigned old_brk = heap->brk;
	unsigned old_page_end;
	unsigned new_page_end;

	top = _top;
	if (top == 0) {
		ret = heap->brk;
	} else if (top >= USER_HEAP_END) {
		ret = heap->brk;
	} else {
		if (top < heap->start_brk)
			top = heap->start_brk;

		old_page_end = (old_brk + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
		new_page_end = (top + PAGE_SIZE - 1) & PAGE_SIZE_MASK;

		if (top > old_brk) {
			if (new_page_end > old_page_end) {
				do_mmap(old_page_end,
					new_page_end - old_page_end,
					PROT_READ | PROT_WRITE | PROT_EXEC,
					MAP_FIXED, -1, 0);
			}
		} else if (top < old_brk) {
			if (new_page_end < old_page_end) {
				do_munmap(new_page_end,
					  old_page_end - new_page_end);
			}
		}

		heap->brk = top;
		ret = top;
	}

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("brk(%x) = %x\n", _top, ret);

	return ret;
}

int sys_sched_yield()
{
	if (TEST_LOG(TEST_LOG_TRACE))
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
	cur->alarm_interval_ms = 0;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("alarm(%u) = %u\n", seconds, remaining);

	return remaining;
}

static unsigned long long timeval_to_ms(const struct timeval *tv)
{
	unsigned long long ms;

	if (!tv)
		return 0;
	if (tv->tv_sec < 0 || tv->tv_usec < 0)
		return 0;

	ms = (unsigned long long)tv->tv_sec * 1000;
	ms += (unsigned long long)tv->tv_usec / 1000;
	if (tv->tv_usec > 0 && ms == 0)
		ms = 1;
	return ms;
}

static void ms_to_itimeval(unsigned long long ms, struct timeval *tv)
{
	if (!tv)
		return;
	tv->tv_sec = (int)(ms / 1000);
	tv->tv_usec = (int)((ms % 1000) * 1000);
}

int sys_setitimer(int which, const struct itimerval *new_value,
		  struct itimerval *old_value)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long long now = time_now_ms();
	unsigned long long new_interval_ms = 0;
	unsigned long long new_value_ms = 0;
	unsigned long long effective_value_ms = 0;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setitimer(%d, %x, %x)\n", which, new_value, old_value);

	if (which != 0)
		return -EINVAL;

	if (old_value) {
		unsigned long long remaining = 0;

		if (cur->alarm_expire_ms > now)
			remaining = cur->alarm_expire_ms - now;
		ms_to_itimeval(cur->alarm_interval_ms, &old_value->it_interval);
		ms_to_itimeval(remaining, &old_value->it_value);
	}

	if (!new_value)
		return 0;

	new_interval_ms = timeval_to_ms(&new_value->it_interval);
	new_value_ms = timeval_to_ms(&new_value->it_value);
	effective_value_ms = new_value_ms;

	cur->alarm_interval_ms = new_interval_ms;
	if (effective_value_ms)
		cur->alarm_expire_ms = now + effective_value_ms;
	else
		cur->alarm_expire_ms = 0;

	return 0;
}

int sys_getitimer(int which, struct itimerval *value)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long long now = time_now_ms();
	unsigned long long remaining = 0;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getitimer(%d, %x)\n", which, value);

	if (which != 0)
		return -EINVAL;
	if (!value)
		return -EFAULT;

	if (cur->alarm_expire_ms > now)
		remaining = cur->alarm_expire_ms - now;

	ms_to_itimeval(cur->alarm_interval_ms, &value->it_interval);
	ms_to_itimeval(remaining, &value->it_value);
	return 0;
}

int sys_getrlimit(int resource, void *limit)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getrlimit\n");

	return sys_ugetrlimit(resource, limit);
}

long sys_personality(unsigned int personality)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("personality\n");

	return PER_LINUX32_3GB;
}

int sys_getgroups(int size, unsigned *list)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getgroups(%d)\n", size);

	/* No supplementary groups — always root. */
	return 0;
}

int sys_setgroups(int size, unsigned short *list)
{
	user_enviroment *u = CURRENT_TASK()->user;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setgroups(%d)\n", size);

	if (u->euid != 0)
		return -EPERM;
	/* We don't track supplementary groups; silently accept. */
	return 0;
}

int sys_getgroups32(int size, unsigned *list)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getgroups32(%d)\n", size);

	/* No supplementary groups — always root. */
	return 0;
}

int sys_setgroups32(int size, unsigned *list)
{
	user_enviroment *u = CURRENT_TASK()->user;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setgroups32(%d)\n", size);

	if (u->euid != 0)
		return -EPERM;
	/* We don't track supplementary groups; silently accept. */
	return 0;
}

int sys_ugetrlimit(int resource, void *limit)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long *rl = (unsigned long *)limit;
	if (!rl)
		return -EFAULT;

	if (rl && resource >= 0 && resource < RLIM_NLIMITS) {
		rl[0] = cur->user->rlimits[resource].rlim_cur;
		rl[1] = cur->user->rlimits[resource].rlim_max;
	}

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("ugetrlimit(%d, rl[0]=%x, rl[1]=%x)\n", resource,
		     rl ? rl[0] : 0, rl ? rl[1] : 0);

	return 0;
}

int sys_setrlimit(int resource, void *limit)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long *rl = (unsigned long *)limit;
	if (!rl)
		return -EFAULT;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("setrlimit(%d, rl[0]=%x, rl[1]=%x)\n", resource,
		     rl ? rl[0] : 0, rl ? rl[1] : 0);

	if (rl && resource >= 0 && resource < RLIM_NLIMITS) {
		cur->user->rlimits[resource].rlim_cur = rl[0];
		cur->user->rlimits[resource].rlim_max = rl[1];
	}
	return 0;
}

/* 32-bit variants — same semantics, just delegate */
int sys_getuid32(void)
{
	return sys_getuid();
}
int sys_getgid32(void)
{
	return sys_getgid();
}
int sys_geteuid32(void)
{
	return sys_geteuid();
}
int sys_getegid32(void)
{
	return sys_getegid();
}
int sys_setuid32(unsigned uid)
{
	return sys_setuid(uid);
}
int sys_setgid32(unsigned gid)
{
	return sys_setgid(gid);
}
int sys_setreuid32(unsigned ruid, unsigned euid)
{
	return sys_setreuid(ruid, euid);
}
int sys_setregid32(unsigned rgid, unsigned egid)
{
	return sys_setregid(rgid, egid);
}
int sys_setresuid32(unsigned r, unsigned e, unsigned s)
{
	return sys_setresuid(r, e, s);
}
int sys_getresuid32(unsigned *r, unsigned *e, unsigned *s)
{
	return sys_getresuid(r, e, s);
}
int sys_setresgid32(unsigned r, unsigned e, unsigned s)
{
	return sys_setresgid(r, e, s);
}
int sys_getresgid32(unsigned *r, unsigned *e, unsigned *s)
{
	return sys_getresgid(r, e, s);
}
int sys_setfsuid32(unsigned fsuid)
{
	return sys_setfsuid(fsuid);
}
int sys_setfsgid32(unsigned fsgid)
{
	return sys_setfsgid(fsgid);
}

int sys_exit_group(int status)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("exit_group(%d)\n", status);

	sys_exit((unsigned)status);
	return 0; /* unreachable */
}

int sys_gettid(void)
{
	return CURRENT_TASK()->psid;
}

int sys_tkill(int tid, int sig)
{
	if (sig <= 0 || sig >= NSIG)
		return -EINVAL;
	return ps_send_signal((unsigned)tid, sig);
}

int sys_set_tid_address(int *tidptr)
{
	task_struct *cur = CURRENT_TASK();

	cur->clear_child_tid = tidptr;
	return cur->psid;
}

int sys_nice(int inc)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("nice(%d)\n", inc);

	/* MOS has no real nice-based scheduling; silently accept. */
	(void)inc;
	return 0;
}

int sys_acct(const char *filename)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("acct(%s)\n", filename ? filename : "(null)");

	if (!filename)
		return 0; /* disabling accounting: no-op */

	return -ENOSYS;
}
