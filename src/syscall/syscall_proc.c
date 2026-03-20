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
		do_mmap(task->user->brk, PAGE_SIZE, PROT_READ | PROT_WRITE, 0,
			-1, 0);
		task->user->brk += PAGE_SIZE;
	}

	if (top == 0) {
		ret = task->user->brk;
	} else if (top >= USER_HEAP_END) {
		ret = task->user->brk;
	} else if (top > task->user->brk) {
		size = top - task->user->brk;
		pages = (size - 1) / PAGE_SIZE + 1;
		do_mmap(task->user->brk, PAGE_SIZE * pages,
			PROT_READ | PROT_WRITE, 0, -1, 0);
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

	new_esp =
		(unsigned char *)((unsigned)frame->esp - sizeof(signal_frame));
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
