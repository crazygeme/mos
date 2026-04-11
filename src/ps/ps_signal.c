/*
 * ps_signal.c — signal delivery, syscall handlers for signal-related syscalls.
 *
 * Covers: ps_send_signal, kill, pause, sigaction, rt_sigaction,
 *         sigprocmask, rt_sigprocmask, sigreturn, rt_sigreturn,
 *         do_signal, sigaltstack, rt_sigpending, rt_sigtimedwait,
 *         rt_sigqueueinfo, rt_sigsuspend.
 */
#include <ps/ps.h>
#include <ps/signal.h>
#include <int/int.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>

void do_signal(intr_frame *frame);

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
	int ret;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("kill(%d, %d)\n", pid, sig);

	if (sig == 0)
		return pid > 0 ? (ps_find_process((unsigned)pid) ? 0 : -ESRCH) :
				 0;

	if (pid > 0) {
		ret = ps_send_signal((unsigned)pid, sig);
		if (ret == 0 && cur->type == ps_user &&
		    (unsigned)pid == cur->psid && cur->signal &&
		    !(cur->signal->sig_mask & (1UL << (sig - 1)))) {
			intr_frame *frame =
				(intr_frame *)((char *)cur + PAGE_SIZE -
					       sizeof(intr_frame));
			frame->eax = 0;
			do_signal(frame);
		}
		return ret;
	}

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

void *sys_signal(int sig, void *handler)
{
	struct sigaction sa;
	struct sigaction old_sa;

	sa.sa_handler = handler;
	sa.sa_mask = 0;
	sa.sa_flags = SA_RESTART;
	sa.sa_restorer = NULL;

	if (sys_sigaction(sig, &sa, &old_sa) < 0)
		return SIG_ERR;

	return old_sa.sa_handler;
}

/*
 * Linux/i386 rt_sigaction uses the kernel layout:
 *   handler, flags, restorer, mask
 *
 * glibc 2.3.2 passes sigsetsize = 8 on this ABI, so the kernel-visible mask
 * payload is just two 32-bit words even though libc's userspace sigset_t is
 * larger. We only support signals 1..31, so we preserve the low word and
 * clear the high word on writeback.
 */
struct rt_sigaction_user {
	void (*sa_handler)(int);
	unsigned long sa_flags;
	void (*sa_restorer)(void);
	unsigned long sa_mask[2];
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
	if (sigsetsize != 8)
		return -EINVAL;

	sa = &cur->signal->sig_handlers[sig];

	if (oact) {
		struct rt_sigaction_user *u = (struct rt_sigaction_user *)oact;
		u->sa_handler = sa->sa_handler;
		u->sa_flags = sa->sa_flags;
		u->sa_restorer = sa->sa_restorer;
		u->sa_mask[0] = sa->sa_mask;
		u->sa_mask[1] = 0;
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
	frame->ds = sf->saved_ds;
	frame->es = sf->saved_es;
	frame->fs = sf->saved_fs;
	frame->gs = sf->saved_gs;

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
	rt_signal_frame *sf =
		(rt_signal_frame *)((unsigned char *)frame->esp - 4);
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

/* Fire SIGALRM if the current task's alarm has expired. */
static void check_alarm(task_struct *cur)
{
	if (!cur->alarm_expire_ms || time_now_ms() < cur->alarm_expire_ms)
		return;

	if (cur->alarm_interval_ms)
		cur->alarm_expire_ms = time_now_ms() + cur->alarm_interval_ms;
	else
		cur->alarm_expire_ms = 0;
	cur->signal->sig_pending |= (1UL << (SIGALRM - 1));
}

/* Return the lowest-numbered deliverable signal, or 0 if none. */
static int pick_signal(task_struct *cur)
{
	unsigned long deliverable = cur->signal->sig_pending &
				    ~cur->signal->sig_mask;
	int sig;

	if (!deliverable)
		return 0;
	for (sig = 1; sig < NSIG; sig++) {
		if (deliverable & (1UL << (sig - 1)))
			return sig;
	}
	return 0;
}

/*
 * Restore the saved sigmask (used after sigsuspend/sigpause).
 * Called when a signal is ignored or has a default-ignore action so
 * the mask is not left as the temporary sigsuspend mask.
 */
static void maybe_restore_sigmask(task_struct *cur)
{
	if (cur->signal->restore_sigmask) {
		cur->signal->sig_mask = cur->signal->saved_sigmask;
		cur->signal->restore_sigmask = 0;
	}
}

/*
 * Handle a signal whose disposition is SIG_DFL.
 * Returns 1 if the signal was fully handled (no frame needed), 0 otherwise.
 */
static int handle_sig_dfl(task_struct *cur, int sig)
{
	switch (sig) {
	case SIGCHLD:
	case SIGURG:
	case SIGWINCH:
	case SIGCONT:
		maybe_restore_sigmask(cur);
		return 1;
	default:
		do_exit(sig);
		return 1;
	}
}

/* Resolve the stack pointer to use for signal frame delivery. */
static unsigned char *resolve_sigstack(task_struct *cur, intr_frame *frame,
				       struct sigaction *sa)
{
	if ((sa->sa_flags & SA_ONSTACK) && cur->signal->altstack.ss_sp &&
	    !(cur->signal->altstack.ss_flags & SS_DISABLE) &&
	    !(cur->signal->altstack.ss_flags & SS_ONSTACK)) {
		cur->signal->altstack.ss_flags |= SS_ONSTACK;
		return (unsigned char *)cur->signal->altstack.ss_sp +
		       cur->signal->altstack.ss_size;
	}
	return (unsigned char *)frame->esp;
}

/* Build the rt (SA_SIGINFO) signal frame on the user stack. */
static void build_rt_frame(task_struct *cur, intr_frame *frame,
			   rt_signal_frame *rt_sf, struct sigaction *sa,
			   int sig)
{
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
}

/* Build the legacy signal frame on the user stack. */
static void build_legacy_frame(task_struct *cur, intr_frame *frame,
			       signal_frame *sf, int sig)
{
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
	sf->saved_ds = frame->ds;
	sf->saved_es = frame->es;
	sf->saved_fs = frame->fs;
	sf->saved_gs = frame->gs;
	/*
	 * If inside sigsuspend, save the pre-sigsuspend mask so sigreturn
	 * restores it (not the temporary sigsuspend mask).
	 */
	sf->saved_mask = cur->signal->restore_sigmask ?
				 cur->signal->saved_sigmask :
				 cur->signal->sig_mask;
	cur->signal->restore_sigmask = 0;
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
	struct sigaction *sa;
	unsigned char *new_esp;
	void (*handler)(int);
	int sig, is_rt;

	if (cur->type != ps_user)
		return;
	if (frame->cs != USER_CODE_SELECTOR)
		return;

	check_alarm(cur);

	sig = pick_signal(cur);
	if (!sig)
		return;
	cur->signal->sig_pending &= ~(1UL << (sig - 1));

	/* SIGKILL / SIGSTOP cannot be caught or ignored. */
	if (sig == SIGKILL || sig == SIGSTOP) {
		sys_exit(sig | 0x80);
		return;
	}

	sa = &cur->signal->sig_handlers[sig];

	if (sa->sa_handler == SIG_IGN) {
		maybe_restore_sigmask(cur);
		return;
	}

	if (sa->sa_handler == SIG_DFL) {
		handle_sig_dfl(cur, sig);
		return;
	}

	/*
	 * Linux/i386 chooses the rt signal frame based on SA_SIGINFO, not on
	 * whether the handler was installed via rt_sigaction().
	 */
	is_rt = (sa->sa_flags & SA_SIGINFO) != 0;

	new_esp = resolve_sigstack(cur, frame, sa);
	new_esp -= is_rt ? sizeof(rt_signal_frame) : sizeof(signal_frame);
	new_esp = (unsigned char *)((unsigned)new_esp &
				    ~0xfU); /* 16-byte align */

	if (is_rt)
		build_rt_frame(cur, frame, (rt_signal_frame *)new_esp, sa, sig);
	else
		build_legacy_frame(cur, frame, (signal_frame *)new_esp, sig);

	/* Block this signal while handler runs (unless SA_NODEFER). */
	if (!(sa->sa_flags & SA_NODEFER))
		cur->signal->sig_mask |= (1UL << (sig - 1));
	cur->signal->sig_mask |= sa->sa_mask;

	handler = sa->sa_handler;
	if (sa->sa_flags & SA_RESETHAND)
		sa->sa_handler = SIG_DFL;

	frame->eip = (void *)handler;
	frame->esp = (void *)new_esp;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("sig_deliver(%d)\n", sig);
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
