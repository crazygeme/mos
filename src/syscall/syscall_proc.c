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
	task_struct *cur = CURRENT_TASK();

	if (TestControl.verbos)
		klog("getpgrp\n");

	return cur->user->group_id;
}

int sys_getpgid(unsigned pid)
{
	task_struct *t;

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
		klog("setsid() = %d\n", cur->psid);

	cur->user->session_id = cur->psid;
	cur->user->group_id = cur->psid;
	return cur->psid;
}

int sys_getuid()
{
	if (TestControl.verbos)
		klog("getuid\n");

	return CURRENT_TASK()->user->uid;
}

int sys_getgid()
{
	if (TestControl.verbos)
		klog("getgid\n");

	return CURRENT_TASK()->user->gid;
}

int sys_geteuid()
{
	if (TestControl.verbos)
		klog("geteuid\n");

	return CURRENT_TASK()->user->euid;
}

int sys_getegid()
{
	if (TestControl.verbos)
		klog("getegid\n");

	return CURRENT_TASK()->user->egid;
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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

	if (TestControl.verbos)
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
		return 0; /* FIXME: wait on group id */
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
	task_struct *target;

	if (sig <= 0 || sig >= NSIG)
		return -EINVAL;

	target = ps_find_process(pid);
	if (!target)
		return -ESRCH;

	target->signal->sig_pending |= (1UL << (sig - 1));

	/* Wake the target if it is sleeping so it can receive the signal. */
	if (target->status == ps_waiting)
		ps_put_to_ready_queue(target);

	return 0;
}

int sys_kill(unsigned pid, int sig)
{
	if (TestControl.verbos)
		klog("kill(%d, %d)\n", pid, sig);

	if (sig == 0)
		return ps_find_process(pid) ? 0 : -ESRCH;

	return ps_send_signal(pid, sig);
}

int sys_brk(unsigned _top)
{
	task_struct *task = CURRENT_TASK();
	unsigned size, pages, top, ret;

	top = _top;
	if (task->user->brk == task->user->start_brk) {
		do_mmap(task->user->brk, PAGE_SIZE,
			PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED, -1, 0);
		task->user->brk += PAGE_SIZE;
	}

	if (top == 0) {
		ret = task->user->brk;
	} else if (top >= USER_HEAP_END) {
		ret = task->user->brk;
	} else if (top > task->user->brk) {
		size = top - task->user->brk;
		pages = (size - 1) / PAGE_SIZE + 1;
		/* MAP_FIXED ensures the mapping lands at exactly brk, preventing
		 * silent fallback to a different address if the range is still
		 * partially occupied after a shrink. */
		do_mmap(task->user->brk, PAGE_SIZE * pages,
			PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED, -1, 0);
		top = task->user->brk + pages * PAGE_SIZE;
		task->user->brk = top;
		ret = top;
	} else {
		if (top < task->user->start_brk)
			top = task->user->start_brk;
		size = task->user->brk - top;
		pages = (size - 1) / PAGE_SIZE + 1;
		do_munmap(top & PAGE_SIZE_MASK, pages * PAGE_SIZE);
		task->user->brk = top;
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
	task_struct *cur = CURRENT_TASK();

	if (TestControl.verbos)
		klog("pause\n");

	/* Yield CPU until a signal becomes deliverable. */
	while (!(cur->signal->sig_pending & ~cur->signal->sig_mask))
		task_sched();

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

	if (TestControl.verbos)
		klog("sigaction(%d, %x, %x)\n", sig, act, oact);

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

	if (TestControl.verbos)
		klog("sigprocmask(%d)\n", how);

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

	if (TestControl.verbos)
		klog("sigreturn\n");

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
	struct sigaction *sa;
	signal_frame *sf;
	unsigned char *new_esp;

	if (cur->type != ps_user)
		return;
	/* Only deliver when returning to user mode. */
	if (frame->cs != USER_CODE_SELECTOR)
		return;

	/* Fire alarm if it has expired. */
	if (cur->alarm_expire_ms && time_now_ms() >= cur->alarm_expire_ms) {
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

	if (sa->sa_handler == SIG_IGN)
		return;

	if (sa->sa_handler == SIG_DFL) {
		switch (sig) {
		/* Signals whose default action is to ignore */
		case SIGCHLD:
		case SIGURG:
		case SIGWINCH:
		case SIGCONT:
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
		new_esp = (unsigned char *)((unsigned)frame->esp -
					    sizeof(signal_frame));
	}
	/* 16-byte align */
	new_esp = (unsigned char *)((unsigned)new_esp & ~0xfU);
	sf = (signal_frame *)new_esp;

	extern void vdso_sigreturn_tramp();

	sf->return_addr = (unsigned int)mm_vdso_translate(vdso_sigreturn_tramp);
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
	sf->saved_mask = cur->signal->sig_mask;

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

	if (TestControl.verbos)
		klog("sig_deliver(%d)\n", sig);
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

int sys_getgroups(int size, unsigned *list)
{
	if (TestControl.verbos)
		klog("getgroups(%d)\n", size);

	/* No supplementary groups — always root. */
	return 0;
}

int sys_setgroups(int size, unsigned short *list)
{
	user_enviroment *u = CURRENT_TASK()->user;

	if (TestControl.verbos)
		klog("setgroups(%d)\n", size);

	if (u->euid != 0)
		return -EPERM;
	/* We don't track supplementary groups; silently accept. */
	return 0;
}

int sys_getgroups32(int size, unsigned *list)
{
	if (TestControl.verbos)
		klog("getgroups32(%d)\n", size);

	/* No supplementary groups — always root. */
	return 0;
}

int sys_setgroups32(int size, unsigned *list)
{
	user_enviroment *u = CURRENT_TASK()->user;

	if (TestControl.verbos)
		klog("setgroups32(%d)\n", size);

	if (u->euid != 0)
		return -EPERM;
	/* We don't track supplementary groups; silently accept. */
	return 0;
}

int sys_ugetrlimit(int resource, void *limit)
{
	unsigned long *rl = (unsigned long *)limit;

	if (TestControl.verbos)
		klog("ugetrlimit(%d)\n", resource);

	if (rl) {
		rl[0] = 0xFFFFFFFFu; /* rlim_cur = RLIM_INFINITY */
		rl[1] = 0xFFFFFFFFu; /* rlim_max = RLIM_INFINITY */
	}
	return 0;
}

int sys_setrlimit(int resource, void *limit)
{
	if (TestControl.verbos)
		klog("setrlimit(%d)\n", resource);

	/* We don't enforce resource limits; silently accept any value. */
	return 0;
}

int sys_sigaltstack(const stack_t *ss, stack_t *old_ss)
{
	task_struct *cur = CURRENT_TASK();
	stack_t *alt = &cur->signal->altstack;

	if (TestControl.verbos)
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
	if (TestControl.verbos)
		klog("exit_group(%d)\n", status);

	sys_exit((unsigned)status);
	return 0; /* unreachable */
}
