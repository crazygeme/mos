/*
 * syscall_proc.c — process management syscall handlers.
 *
 * Covers: getpid/ppid/pgrp, uid/gid, setsid, wait4, kill, brk,
 *         sched_yield, alarm, pause, signals, rlimit, personality.
 */
#include <ps/ps.h>
#include <ps/signal.h>
#include <fs/fs.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/vdso.h>
#include <hw/time.h>
#include <int/int.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include "syscall_internal.h"

extern unsigned long long gdt[];

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

struct kill_all_ctx {
	task_struct *self;
	int sig;
	unsigned pgrp;
	int sent;
};

static int can_send_signal(task_struct *sender, task_struct *target)
{
	user_enviroment *su, *tu;

	if (!sender || !target || !target->user)
		return 0;

	su = sender->user;
	tu = target->user;
	if (!su)
		return 0;
	if (su->euid == 0)
		return 1;

	return su->uid == tu->uid || su->uid == tu->suid ||
	       su->euid == tu->uid || su->euid == tu->suid;
}

static void send_if_other(task_struct *task, void *opaque)
{
	struct kill_all_ctx *c = opaque;

	if (task->type != ps_user || task->psid == c->self->psid)
		return;
	if (ps_send_signal(task->psid, c->sig) == 0)
		c->sent++;
}

static void send_if_pgrp(task_struct *task, void *opaque)
{
	struct kill_all_ctx *c = opaque;

	if (task->type != ps_user || !task->user || task->psid == c->self->psid)
		return;
	if (task->user->group_id != c->pgrp)
		return;
	if (ps_send_signal(task->psid, c->sig) == 0)
		c->sent++;
}

int sys_getpid()
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getpid() = %d\n", cur->psid);

	return cur->psid;
}

int sys_getppid()
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getppid() = %d\n", cur->parent->psid);

	return cur->parent->psid;
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

/*
 * ps_send_signal — deliver signal sig to process pid.
 * Sets the pending bit and wakes the target if it is blocked.
 * Called by sys_kill and internally (e.g. SIGCHLD on child exit).
 */
int ps_send_signal(unsigned pid, int sig)
{
	task_struct *sender = CURRENT_TASK();
	task_struct *target;

	if (sig <= 0 || sig >= NSIG)
		return -EINVAL;

	target = ps_find_process(pid);
	if (!target)
		return -ESRCH;

	if (target->type != ps_user || !target->user || !target->signal) {
		return -ESRCH;
	}

	if (!can_send_signal(sender, target))
		return -EPERM;

	target->signal->sig_pending |= (1UL << (sig - 1));

	if (target->status == ps_waiting &&
	    !(target->signal->sig_mask & (1UL << (sig - 1))))
		ps_put_to_ready_queue(target);

	return 0;
}

int sys_kill(int pid, int sig)
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("kill(%d, %d)\n", pid, sig);

	if (sig == 0)
		return pid > 0 ? (ps_find_process((unsigned)pid) ? 0 : -ESRCH) :
				 0;

	if (pid > 0)
		return ps_send_signal((unsigned)pid, sig);

	if (pid == 0) {
		struct kill_all_ctx ctx;
		if (!cur->user)
			return -ESRCH;
		ctx.self = cur;
		ctx.sig = sig;
		ctx.pgrp = cur->user->group_id;
		ctx.sent = 0;
		ps_enum_all(send_if_pgrp, &ctx);
		return ctx.sent ? 0 : -ESRCH;
	}

	if (pid == -1) {
		struct kill_all_ctx ctx;
		ctx.self = cur;
		ctx.sig = sig;
		ctx.pgrp = 0;
		ctx.sent = 0;
		ps_enum_all(send_if_other, &ctx);
		return ctx.sent ? 0 : -ESRCH;
	}

	{
		struct kill_all_ctx ctx;
		ctx.self = cur;
		ctx.sig = sig;
		ctx.pgrp = (unsigned)(-pid);
		ctx.sent = 0;
		ps_enum_all(send_if_pgrp, &ctx);
		return ctx.sent ? 0 : -ESRCH;
	}
}

int sys_brk(unsigned _top)
{
	task_struct *task = CURRENT_TASK();
	unsigned top, ret;
	unsigned old_brk = task->user->brk;
	unsigned old_page_end;
	unsigned new_page_end;

	top = _top;
	if (top == 0) {
		ret = task->user->brk;
	} else if (top >= USER_HEAP_END) {
		ret = task->user->brk;
	} else {
		if (top < task->user->start_brk)
			top = task->user->start_brk;

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

		task->user->brk = top;
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

int sys_pause()
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("pause\n");

	while (!(cur->signal->sig_pending & ~cur->signal->sig_mask))
		time_wait(0);

	return -EINTR;
}

int sys_sigaction(int sig, void *act, void *oact)
{
	task_struct *cur = CURRENT_TASK();
	struct sigaction *sa;

	if (sig <= 0 || sig >= NSIG)
		return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP)
		return -EINVAL;

	sa = &cur->signal->sig_handlers[sig];

	if (oact)
		*(struct sigaction *)oact = *sa;
	if (act)
		*sa = *(struct sigaction *)act;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("sigaction(%d, %x, %x)\n", sig, act, oact);

	return 0;
}

/*
 * glibc 2.3.2 on Linux/i386 passes rt_sigaction using the userspace layout:
 *   handler, sigset_t mask, flags, restorer
 *
 * The sigsetsize argument is still 8 on this ABI.  We only support signals
 * 1..31, so we translate the low word and zero the remaining words on
 * writeback.
 */
#define RT_SIGSET_WORDS 32
struct rt_sigaction_user {
	void (*sa_handler)(int);
	unsigned long sa_mask[RT_SIGSET_WORDS];
	unsigned long sa_flags;
	void (*sa_restorer)(void);
};

typedef struct _rt_siginfo_user {
	int si_signo;
	int si_errno;
	int si_code;
	int _pad[(128 / sizeof(int)) - 3];
} rt_siginfo_user;

typedef struct _rt_sigcontext_user {
	unsigned short gs, __gsh;
	unsigned short fs, __fsh;
	unsigned short es, __esh;
	unsigned short ds, __dsh;
	unsigned long edi;
	unsigned long esi;
	unsigned long ebp;
	unsigned long esp;
	unsigned long ebx;
	unsigned long edx;
	unsigned long ecx;
	unsigned long eax;
	unsigned long trapno;
	unsigned long err;
	unsigned long eip;
	unsigned short cs, __csh;
	unsigned long eflags;
	unsigned long esp_at_signal;
	unsigned short ss, __ssh;
	void *fpstate;
	unsigned long oldmask;
	unsigned long cr2;
} rt_sigcontext_user;

typedef struct _rt_ucontext_user {
	unsigned long uc_flags;
	struct _rt_ucontext_user *uc_link;
	stack_t uc_stack;
	rt_sigcontext_user uc_mcontext;
	sigset_t uc_sigmask;
} rt_ucontext_user;

typedef struct _rt_fpreg_user {
	unsigned short significand[4];
	unsigned short exponent;
} rt_fpreg_user;

typedef struct _rt_fpstate_user {
	unsigned long cw;
	unsigned long sw;
	unsigned long tag;
	unsigned long ipoff;
	unsigned long cssel;
	unsigned long dataoff;
	unsigned long datasel;
	rt_fpreg_user _st[8];
	unsigned long status;
} rt_fpstate_user;

typedef struct _rt_signal_frame {
	unsigned int pretcode;
	int sig;
	unsigned int pinfo;
	unsigned int puc;
	rt_siginfo_user info;
	rt_ucontext_user uc;
	rt_fpstate_user fpstate;
	unsigned char retcode[8];
} rt_signal_frame;

int sys_rt_sigaction(int sig, void *act, void *oact, unsigned sigsetsize)
{
	task_struct *cur = CURRENT_TASK();
	struct sigaction *sa;

	if (sig <= 0 || sig >= NSIG)
		return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP)
		return -EINVAL;

	sa = &cur->signal->sig_handlers[sig];

	if (oact) {
		struct rt_sigaction_user *u = (struct rt_sigaction_user *)oact;
		int i;
		u->sa_handler = sa->sa_handler;
		u->sa_flags = sa->sa_flags;
		u->sa_restorer = sa->sa_restorer;
		for (i = 0; i < RT_SIGSET_WORDS; i++)
			u->sa_mask[i] = 0;
		u->sa_mask[0] = sa->sa_mask;
	}
	if (act) {
		struct rt_sigaction_user *u = (struct rt_sigaction_user *)act;
		sa->sa_handler = u->sa_handler;
		sa->sa_flags = u->sa_flags;
		sa->sa_restorer = u->sa_restorer;
		sa->sa_mask = (unsigned long)u->sa_mask[0];
	}

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("rt_sigaction(%d, %x, %x)\n", sig, act, oact);

	return 0;
}

int sys_sigprocmask(int how, void *set, void *oset)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long newmask;

	if (oset)
		*(unsigned long *)oset = cur->signal->sig_mask;

	if (!set)
		return 0;

	newmask = *(unsigned long *)set;

	switch (how) {
	case SIG_BLOCK:
		cur->signal->sig_mask |= newmask;
		break;
	case SIG_UNBLOCK:
		cur->signal->sig_mask &= ~newmask;
		break;
	case SIG_SETMASK:
		cur->signal->sig_mask = newmask;
		break;
	default:
		return -EINVAL;
	}

	/* SIGKILL and SIGSTOP can never be blocked. */
	cur->signal->sig_mask &=
		~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("sigprocmask(%d)\n", how);

	return 0;
}

/*
 * rt_sigprocmask (syscall 175).
 * The mask pointer points to an 8-byte (64-signal) kernel sigset_t.
 * We only use the low 32 bits; write 8 bytes to oset so glibc sees
 * a fully initialised mask (upper 4 bytes = 0).
 */
int sys_rt_sigprocmask(int how, void *set, void *oset, unsigned sigsetsize)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long newmask;

	if (oset) {
		((unsigned long *)oset)[0] = cur->signal->sig_mask;
		((unsigned long *)oset)[1] = 0;
	}

	if (!set)
		return 0;

	newmask = ((unsigned long *)set)[0];

	switch (how) {
	case SIG_BLOCK:
		cur->signal->sig_mask |= newmask;
		break;
	case SIG_UNBLOCK:
		cur->signal->sig_mask &= ~newmask;
		break;
	case SIG_SETMASK:
		cur->signal->sig_mask = newmask;
		break;
	default:
		return -EINVAL;
	}

	/* SIGKILL and SIGSTOP can never be blocked. */
	cur->signal->sig_mask &=
		~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("rt_sigprocmask(%d)\n", how);

	return 0;
}

/*
 * sys_sigreturn — restore user context after a signal handler returns.
 *
 * The signal handler's trampoline called "int $0x80" with eax=119 after
 * the handler's "ret" popped return_addr from the stack.  At that point:
 *   user ESP = signal_frame_base + 4   (return_addr has been popped)
 * so the saved frame is at (frame->esp - 4).
 */
int sys_sigreturn()
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	signal_frame *sf = (signal_frame *)((unsigned char *)frame->esp - 4);

	/* Restore original user registers and EIP into the interrupt frame. */
	frame->eip = (void *)sf->saved_eip;
	frame->eflags = sf->saved_eflags;
	frame->esp = (void *)sf->saved_esp;
	frame->eax = sf->saved_eax;
	frame->ebx = sf->saved_ebx;
	frame->ecx = sf->saved_ecx;
	frame->edx = sf->saved_edx;
	frame->esi = sf->saved_esi;
	frame->edi = sf->saved_edi;
	frame->ebp = sf->saved_ebp;

	/* Restore the signal mask saved before delivery. */
	cur->signal->sig_mask = sf->saved_mask;

	/*
	 * If we delivered on the altstack, check whether the restored esp
	 * falls outside the altstack range; if so we're leaving it.
	 */
	{
		stack_t *alt = &cur->signal->altstack;
		if (alt->ss_flags & SS_ONSTACK) {
			unsigned alt_base = (unsigned)alt->ss_sp;
			unsigned alt_top = alt_base + alt->ss_size;
			unsigned restored = sf->saved_esp;
			if (restored < alt_base || restored >= alt_top)
				alt->ss_flags &= ~SS_ONSTACK;
		}
	}

	/*
	 * Return the original eax so that the interrupted syscall's result
	 * is preserved when execution resumes.  syscall_process will write
	 * this into frame->eax, but do_signal (called afterwards) will see
	 * the restored frame and may deliver the next queued signal.
	 */
	return (int)sf->saved_eax;
}

int sys_rt_sigreturn()
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	rt_signal_frame *sf = (rt_signal_frame *)((unsigned char *)frame->esp - 4);
	rt_sigcontext_user *sc = &sf->uc.uc_mcontext;

	frame->eip = (void *)sc->eip;
	frame->eflags = sc->eflags;
	frame->esp = (void *)sc->esp_at_signal;
	frame->eax = sc->eax;
	frame->ebx = sc->ebx;
	frame->ecx = sc->ecx;
	frame->edx = sc->edx;
	frame->esi = sc->esi;
	frame->edi = sc->edi;
	frame->ebp = sc->ebp;
	frame->ds = sc->ds;
	frame->es = sc->es;
	frame->fs = sc->fs;
	frame->gs = sc->gs;
	cur->signal->sig_mask = sf->uc.uc_sigmask;

	{
		stack_t *alt = &cur->signal->altstack;
		if (alt->ss_flags & SS_ONSTACK) {
			unsigned alt_base = (unsigned)alt->ss_sp;
			unsigned alt_top = alt_base + alt->ss_size;
			unsigned restored = sc->esp_at_signal;
			if (restored < alt_base || restored >= alt_top)
				alt->ss_flags &= ~SS_ONSTACK;
		}
	}

	return (int)sc->eax;
}

static void build_rt_sigreturn_code(unsigned char retcode[8])
{
	retcode[0] = 0xb8; /* mov imm32,%eax */
	*(unsigned int *)&retcode[1] = 173;
	retcode[5] = 0xcd; /* int $0x80 */
	retcode[6] = 0x80;
	retcode[7] = 0x90; /* nop */
}

static void build_sigreturn_code(unsigned char retcode[8])
{
	retcode[0] = 0xb8; /* mov imm32,%eax */
	*(unsigned int *)&retcode[1] = 119;
	retcode[5] = 0xcd; /* int $0x80 */
	retcode[6] = 0x80;
	retcode[7] = 0x90; /* nop */
}

/*
 * do_signal — check for deliverable signals and redirect the interrupt frame
 * to the handler before iret returns to user space.
 *
 * Called at the end of syscall_process() and from the timer interrupt path.
 * Only acts when the frame is returning to user mode (cs == USER_CODE_SELECTOR).
 */
void do_signal(intr_frame *frame)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long deliverable;
	int sig;
	int is_rt;
	struct sigaction *sa;
	signal_frame *sf;
	rt_signal_frame *rt_sf;
	unsigned char *new_esp;

	if (cur->type != ps_user)
		return;
	/* Only deliver when returning to user mode. */
	if (frame->cs != USER_CODE_SELECTOR)
		return;

	/* Fire alarm if it has expired. */
	if (cur->alarm_expire_ms && time_now_ms() >= cur->alarm_expire_ms) {
		if (cur->alarm_interval_ms)
			cur->alarm_expire_ms =
				time_now_ms() + cur->alarm_interval_ms;
		else
			cur->alarm_expire_ms = 0;
		cur->signal->sig_pending |= (1UL << (SIGALRM - 1));
	}

	deliverable = cur->signal->sig_pending & ~cur->signal->sig_mask;
	if (!deliverable)
		return;

	/* Pick the lowest-numbered deliverable signal. */
	for (sig = 1; sig < NSIG; sig++) {
		if (deliverable & (1UL << (sig - 1)))
			break;
	}
	if (sig >= NSIG)
		return;

	cur->signal->sig_pending &= ~(1UL << (sig - 1));

	/* SIGKILL / SIGSTOP cannot be caught or ignored. */
	if (sig == SIGKILL || sig == SIGSTOP) {
		sys_exit(sig | 0x80);
		return;
	}

	sa = &cur->signal->sig_handlers[sig];
	/*
	 * Linux/i386 chooses the rt signal frame based on SA_SIGINFO, not on
	 * whether the handler was installed via rt_sigaction().  A plain
	 * one-argument handler installed through rt_sigaction still receives
	 * the legacy frame/sigreturn ABI.
	 */
	is_rt = (sa->sa_flags & SA_SIGINFO) != 0;

	if (sa->sa_handler == SIG_IGN) {
		if (cur->signal->restore_sigmask) {
			cur->signal->sig_mask = cur->signal->saved_sigmask;
			cur->signal->restore_sigmask = 0;
		}
		return;
	}

	if (sa->sa_handler == SIG_DFL) {
		switch (sig) {
		/* Signals whose default action is to ignore */
		case SIGCHLD:
		case SIGURG:
		case SIGWINCH:
		case SIGCONT:
			if (cur->signal->restore_sigmask) {
				cur->signal->sig_mask =
					cur->signal->saved_sigmask;
				cur->signal->restore_sigmask = 0;
			}
			return;
		default:
			/* Terminate: signal number in bits 0-6, already POSIX-encoded. */
			do_exit(sig);
			return;
		}
	}

	/* ---- User-defined handler: build signal frame on user stack ---- */

	/*
	 * SA_ONSTACK: deliver on the alternate signal stack if one is
	 * registered, enabled, and we are not already running on it.
	 */
	if ((sa->sa_flags & SA_ONSTACK) && cur->signal->altstack.ss_sp &&
	    !(cur->signal->altstack.ss_flags & SS_DISABLE) &&
	    !(cur->signal->altstack.ss_flags & SS_ONSTACK)) {
		new_esp = (unsigned char *)cur->signal->altstack.ss_sp +
			  cur->signal->altstack.ss_size;
		cur->signal->altstack.ss_flags |= SS_ONSTACK;
	} else {
		new_esp = (unsigned char *)frame->esp;
	}
	new_esp -= is_rt ? sizeof(rt_signal_frame) : sizeof(signal_frame);
	/* 16-byte align */
	new_esp = (unsigned char *)((unsigned)new_esp & ~0xfU);
	sf = (signal_frame *)new_esp;
	rt_sf = (rt_signal_frame *)new_esp;

	if (is_rt) {
		rt_sigcontext_user *sc = &rt_sf->uc.uc_mcontext;
		unsigned long saved_mask = cur->signal->restore_sigmask ?
						   cur->signal->saved_sigmask :
						   cur->signal->sig_mask;

		if ((sa->sa_flags & SA_RESTORER) && sa->sa_restorer) {
			rt_sf->pretcode = (unsigned int)sa->sa_restorer;
		} else {
			build_rt_sigreturn_code(rt_sf->retcode);
			rt_sf->pretcode = (unsigned int)&rt_sf->retcode[0];
		}
		rt_sf->sig = sig;
		rt_sf->pinfo = (unsigned int)&rt_sf->info;
		rt_sf->puc = (unsigned int)&rt_sf->uc;
		memset(&rt_sf->info, 0, sizeof(rt_sf->info));
		rt_sf->info.si_signo = sig;
		rt_sf->uc.uc_flags = 0;
		rt_sf->uc.uc_link = NULL;
		rt_sf->uc.uc_stack = cur->signal->altstack;
		rt_sf->uc.uc_sigmask = saved_mask;
		memset(&rt_sf->fpstate, 0, sizeof(rt_sf->fpstate));
		memset(sc, 0, sizeof(*sc));
		sc->gs = frame->gs;
		sc->fs = frame->fs;
		sc->es = frame->es;
		sc->ds = frame->ds;
		sc->edi = frame->edi;
		sc->esi = frame->esi;
		sc->ebp = frame->ebp;
		sc->esp = (unsigned long)frame->esp;
		sc->ebx = frame->ebx;
		sc->edx = frame->edx;
		sc->ecx = frame->ecx;
		sc->eax = frame->eax;
		sc->trapno = 0;
		sc->err = frame->error_code;
		sc->eip = (unsigned long)frame->eip;
		sc->cs = frame->cs;
		sc->eflags = frame->eflags;
		sc->esp_at_signal = (unsigned long)frame->esp;
		sc->ss = frame->ss;
		sc->fpstate = &rt_sf->fpstate;
		sc->oldmask = saved_mask;
		sc->cr2 = 0;
		cur->signal->restore_sigmask = 0;
	} else {
		build_sigreturn_code(sf->trampoline);
		sf->return_addr = (unsigned int)&sf->trampoline[0];
		sf->signo = sig;
		sf->saved_eip = (unsigned int)frame->eip;
		sf->saved_eflags = frame->eflags;
		sf->saved_esp = (unsigned int)frame->esp;
		sf->saved_eax = frame->eax;
		sf->saved_ebx = frame->ebx;
		sf->saved_ecx = frame->ecx;
		sf->saved_edx = frame->edx;
		sf->saved_esi = frame->esi;
		sf->saved_edi = frame->edi;
		sf->saved_ebp = frame->ebp;
		/*
		 * If inside sigsuspend, save the pre-sigsuspend mask so sigreturn
		 * restores it (not the temporary sigsuspend mask).
		 */
		sf->saved_mask = cur->signal->restore_sigmask ?
					 cur->signal->saved_sigmask :
					 cur->signal->sig_mask;
		cur->signal->restore_sigmask = 0;
	}

	/* Block this signal while handler runs (unless SA_NODEFER). */
	if (!(sa->sa_flags & SA_NODEFER))
		cur->signal->sig_mask |= (1UL << (sig - 1));
	cur->signal->sig_mask |= sa->sa_mask;

	/* Capture handler before SA_RESETHAND can overwrite it. */
	void (*handler)(int) = sa->sa_handler;

	/* SA_RESETHAND: reset disposition to SIG_DFL after first delivery. */
	if (sa->sa_flags & SA_RESETHAND)
		sa->sa_handler = SIG_DFL;

	/* Redirect iret to the signal handler. */
	frame->eip = (void *)handler;
	frame->esp = (void *)new_esp;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("sig_deliver(%d)\n", sig);
}

int sys_getrlimit(int resource, void *limit)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getrlimit\n");

	return -1;
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

int sys_sigaltstack(const stack_t *ss, stack_t *old_ss)
{
	task_struct *cur = CURRENT_TASK();
	stack_t *alt = &cur->signal->altstack;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("sigaltstack\n");

	if (old_ss)
		*old_ss = *alt;

	if (ss) {
		/* Cannot change altstack while executing on it. */
		if (alt->ss_flags & SS_ONSTACK)
			return -EPERM;

		if (ss->ss_flags & SS_DISABLE) {
			alt->ss_sp = 0;
			alt->ss_size = 0;
			alt->ss_flags = SS_DISABLE;
		} else {
			if (ss->ss_size < MINSIGSTKSZ)
				return -ENOMEM;
			alt->ss_sp = ss->ss_sp;
			alt->ss_size = ss->ss_size;
			alt->ss_flags = 0;
		}
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

/*
 * rt_sigpending (176) — return the set of pending blocked signals.
 *
 * Only signals that are both pending and blocked are returned; a pending but
 * unblocked signal would have already been delivered.
 */
int sys_rt_sigpending(sigset_t *set, unsigned sigsetsize)
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("rt_sigpending\n");

	if (!set)
		return -EFAULT;

	*set = cur->signal->sig_pending & cur->signal->sig_mask;
	return 0;
}

/*
 * rt_sigtimedwait (177) — wait for a signal from @set to become pending.
 *
 * Signals in @set should normally be blocked by the caller so they queue
 * rather than being delivered asynchronously.  When a signal in @set
 * becomes pending it is consumed and its number is returned.
 *
 * If @timeout is non-NULL the call returns -EAGAIN after the deadline.
 * Returns -EINTR if a signal not in @set and not blocked becomes deliverable.
 */
int sys_rt_sigtimedwait(const sigset_t *set, void *info,
			const struct timespec *timeout, unsigned sigsetsize)
{
	task_struct *cur = CURRENT_TASK();
	sigset_t wait_set;
	unsigned long long deadline = 0;
	int has_timeout = 0;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("rt_sigtimedwait\n");

	if (!set)
		return -EFAULT;

	wait_set = *set;
	wait_set &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));

	if (timeout) {
		deadline = time_now_ms() +
			   (unsigned long long)timeout->tv_sec * 1000 +
			   (unsigned long long)timeout->tv_nsec / 1000000;
		has_timeout = 1;
	}

	for (;;) {
		sigset_t pending = cur->signal->sig_pending & wait_set;

		if (pending) {
			int sig;

			for (sig = 1; sig < NSIG; sig++) {
				if (pending & (1UL << (sig - 1))) {
					cur->signal->sig_pending &=
						~(1UL << (sig - 1));
					if (info)
						memset(info, 0, sigsetsize);
					return sig;
				}
			}
		}

		/* A non-waited, unblocked signal interrupts the wait. */
		if (cur->signal->sig_pending & ~cur->signal->sig_mask &
		    ~wait_set)
			return -EINTR;

		if (has_timeout) {
			unsigned long long now = time_now_ms();

			if (now >= deadline)
				return -EAGAIN;
			time_wait((unsigned)(deadline - now));
		} else {
			time_wait(0);
		}
	}
}

/*
 * rt_sigqueueinfo (178) — send a signal with siginfo to a process.
 *
 * We don't queue siginfo payloads; just deliver the signal.
 */
int sys_rt_sigqueueinfo(unsigned pid, int sig, void *uinfo)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("rt_sigqueueinfo(%d, %d)\n", pid, sig);

	if (sig == 0)
		return ps_find_process(pid) ? 0 : -ESRCH;

	return ps_send_signal(pid, sig);
}

/*
 * rt_sigsuspend (179) — atomically set signal mask and suspend until a signal.
 *
 * Replaces sig_mask with @mask, sleeps until a signal becomes deliverable,
 * then restores the old mask.  Always returns -EINTR.
 */
int sys_rt_sigsuspend(const sigset_t *mask, unsigned sigsetsize)
{
	task_struct *cur = CURRENT_TASK();
	sigset_t saved_mask, new_mask;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("rt_sigsuspend\n");

	if (!mask)
		return -EFAULT;

	saved_mask = cur->signal->sig_mask;
	new_mask = *mask;
	new_mask &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));
	cur->signal->sig_mask = new_mask;

	/*
	 * ps_signal_wait() checks the condition under ps_lock before sleeping,
	 * closing the race where a signal arrives after the check but before the
	 * task enters the wait queue.  ps_send_signal() now skips waking tasks
	 * whose mask blocks the arriving signal, so we are only woken when an
	 * unmasked signal is pending — no loop needed.
	 */
	ps_signal_wait();

	/*
	 * Do NOT restore sig_mask here.  Leave the sigsuspend mask active so
	 * do_signal() can clear/deliver the pending signal under the correct
	 * mask.  do_signal() will restore saved_mask after handling the signal.
	 */
	cur->signal->saved_sigmask = saved_mask;
	cur->signal->restore_sigmask = 1;
	return -EINTR;
}

/* ── sys_set_thread_area ─────────────────────────────────────────────────── */

struct user_desc {
	unsigned int entry_number;
	unsigned int base_addr;
	unsigned int limit;
	unsigned int seg_32bit : 1;
	unsigned int contents : 2;
	unsigned int read_exec_only : 1;
	unsigned int limit_in_pages : 1;
	unsigned int seg_not_present : 1;
	unsigned int useable : 1;
	unsigned int empty : 25;
};

static unsigned long long build_tls_desc(struct user_desc *u)
{
	unsigned int type;

	if (u->contents >= 2)
		type = 8 | ((u->contents & 1) << 2) |
		       (u->read_exec_only ? 0 : 2);
	else
		type = (u->contents << 2) | (u->read_exec_only ? 0 : 2);

	return MAKE_SEG_DESC(u->base_addr, u->limit, SEG_CLASS_DATA, type,
			     USER_PRIVILEGE,
			     u->limit_in_pages ? SEG_BASE_4K : SEG_BASE_1);
}

int sys_set_thread_area(void *info)
{
	task_struct *cur = CURRENT_TASK();
	unsigned int entry;
	unsigned long long desc;
	struct user_desc *u_info = (struct user_desc *)info;

	if (!u_info)
		return -EFAULT;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("set_thread_area(entry=%d, base=%x)\n",
		     u_info->entry_number, u_info->base_addr);

	entry = u_info->entry_number;

	if (entry == (unsigned int)-1) {
		for (entry = GDT_ENTRY_TLS_MIN; entry <= GDT_ENTRY_TLS_MAX;
		     entry++) {
			if (!cur->user->tls_desc[entry - GDT_ENTRY_TLS_MIN])
				break;
		}
		if (entry > GDT_ENTRY_TLS_MAX)
			return -ESRCH;
		u_info->entry_number = entry;
	}

	if (entry < GDT_ENTRY_TLS_MIN || entry > GDT_ENTRY_TLS_MAX)
		return -EINVAL;

	desc = build_tls_desc(u_info);
	cur->user->tls_desc[entry - GDT_ENTRY_TLS_MIN] = desc;
	gdt[entry] = desc;

	return 0;
}
