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
#include <device/time.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <int/interrupt.h>
#include "ps_internal.h"

void ps_timer_notify(unsigned tid, int signo, int timer_id, uintptr_t value)
{
	task_struct *target;
	int irq;

	spinlock_lock(&ps_lock, &irq);
	target = ps_find_process_unsafe(tid);
	if (target && target->type == ps_user && target->signal) {
		if (signo == SIGRTMIN_KERNEL) {
			target->signal->timer_signal_id = timer_id;
			target->signal->timer_signal_value = value;
		}
		ps_queue_signal_unsafe(target, signo);
	}
	spinlock_unlock(&ps_lock, irq);
}

void do_signal(intr_frame *frame);

#define STOP_SIGNALS_MASK                                  \
	((1UL << (SIGSTOP - 1)) | (1UL << (SIGTSTP - 1)) | \
	 (1UL << (SIGTTIN - 1)) | (1UL << (SIGTTOU - 1)))

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

unsigned long ps_interrupting_signals(task_struct *task)
{
	signal_context *signal = task->signal;
	unsigned long pending;
	int sig;

	if (!signal)
		return 0;
	pending = signal->sig_pending & ~signal->sig_mask;
	if (!pending)
		return 0;
	for (sig = 1; sig < NSIG; sig++) {
		void (*handler)(int);
		unsigned long bit = 1UL << (sig - 1);

		if (!(pending & bit))
			continue;
		handler = signal->sig_handlers[sig].sa_handler;
		if (sig != SIGKILL && sig != SIGSTOP &&
		    (handler == SIG_IGN ||
		     (handler == SIG_DFL &&
		      (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH ||
		       sig == SIGCONT))))
			pending &= ~bit;
	}
	return pending;
}

/* Queue a validated signal while holding ps_lock. */
void ps_queue_signal_unsafe(task_struct *target, int sig)
{
	unsigned long bit;

	bit = 1UL << (sig - 1);
	if (sig == SIGCONT)
		target->signal->sig_pending &= ~STOP_SIGNALS_MASK;
	else if (bit & STOP_SIGNALS_MASK)
		target->signal->sig_pending &= ~(1UL << (SIGCONT - 1));

	target->signal->sig_pending |= bit;

	if (target->status == ps_stopped &&
	    (sig == SIGCONT || sig == SIGKILL)) {
		target->stop_signal = 0;
		target->stop_report_pending = 0;
		ps_put_to_ready_queue_unsafe(target);
	} else if (target->status == ps_waiting && target->wait_interruptible &&
		   (ps_interrupting_signals(target) ||
		    (target->signal_wait_mask & bit))) {
		ps_put_to_ready_queue_unsafe(target);
	}
}

/* Check sender permissions, queue the signal, and wake eligible recipients. */
int ps_send_signal(unsigned pid, int sig)
{
	task_struct *sender = CURRENT_TASK();
	task_struct *target;
	int irq;
	int ret = 0;

	if (sig < 0 || sig >= NSIG)
		return -EINVAL;

	spinlock_lock(&ps_lock, &irq);
	target = ps_find_process_unsafe(pid);
	if (!target) {
		ret = -ESRCH;
		goto done;
	}

	if (target->type != ps_user || !target->user || !target->signal) {
		ret = -ESRCH;
		goto done;
	}

	if (!can_send_signal(sender, target)) {
		ret = -EPERM;
		goto done;
	}

	if (sig)
		ps_queue_signal_unsafe(target, sig);

done:
	spinlock_unlock(&ps_lock, irq);
	return ret;
}

int sys_kill(int pid, int sig)
{
	task_struct *cur = CURRENT_TASK();
	int ret;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("kill(%d, %d)\n", pid, sig);

	if (sig == 0)
		return pid > 0 ? (ps_find_process((unsigned)(uintptr_t)pid) ?
					  0 :
					  -ESRCH) :
				 0;

	if (pid > 0) {
		ret = ps_send_signal((unsigned)(uintptr_t)pid, sig);
		if (ret == 0 && cur->type == ps_user &&
		    (unsigned)(uintptr_t)pid == cur->psid && cur->signal &&
		    !(cur->signal->sig_mask & (1UL << (sig - 1)))) {
			intr_frame *frame =
				(intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
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

	struct kill_all_ctx ctx;
	ctx.self = cur;
	ctx.sig = sig;
	ctx.pgrp = (unsigned)(-pid);
	ctx.sent = 0;
	ps_enum_all(send_if_pgrp, &ctx);
	return ctx.sent ? 0 : -ESRCH;
}

int sys_pause()
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("pause\n");

	while (!ps_interrupting_signals(cur))
		ps_signal_wait();

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

int sys_sigprocmask(int how, void *set, void *oset)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long newmask;

	if (oset)
		*(uint32_t *)oset = cur->signal->sig_mask;

	if (!set)
		return 0;

	newmask = *(uint32_t *)set;

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
		((uint32_t *)oset)[0] = cur->signal->sig_mask;
		((uint32_t *)oset)[1] = 0;
	}

	if (!set)
		return 0;

	newmask = ((uint32_t *)set)[0];

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

/* Return the lowest-numbered deliverable signal, or 0 if none. */
static int pick_signal(task_struct *cur)
{
	unsigned long unmasked = cur->signal->sig_pending &
				 ~cur->signal->sig_mask;
	unsigned long deliverable = ps_interrupting_signals(cur);
	int sig;

	/* An ignored low-numbered signal must not hide the handler for the
	 * signal which just interrupted a syscall. Keep masked signals pending.
	 */
	cur->signal->sig_pending &= ~(unmasked & ~deliverable);
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
static int handle_sig_dfl(task_struct *cur, intr_frame *frame, int sig)
{
	/*
	 * Linux protects global init from ordinary default signal actions.
	 * SysV killall5 broadcasts SIGSTOP/SIGKILL during shutdown and relies
	 * on PID 1 surviving; init catches the signals it intentionally handles.
	 */
	if (cur->psid == 1) {
		maybe_restore_sigmask(cur);
		return 1;
	}

	switch (sig) {
	case SIGCHLD:
	case SIGURG:
	case SIGWINCH:
	case SIGCONT:
		maybe_restore_sigmask(cur);
		return 1;
	case SIGSTOP:
	case SIGTSTP:
	case SIGTTIN:
	case SIGTTOU:
		maybe_restore_sigmask(cur);
		ps_stop_current(frame, sig);
		return 1;
	default:
		do_group_exit(sig);
		return 1;
	}
}

/*
 * do_signal — check for deliverable signals and redirect the interrupt frame
 * to the handler before iret returns to user space.
 *
 * Called at the end of syscall_process() and from the timer interrupt path.
 * Only acts when the architecture reports a return to user mode.
 */
void do_signal(intr_frame *frame)
{
	task_struct *cur = CURRENT_TASK();
	struct sigaction *sa;
	int sig;

	if (cur->type != ps_user)
		return;
	if (!arch_interrupt_frame_is_user(frame))
		return;

next_signal:
	sig = pick_signal(cur);
	if (!sig)
		return;
	cur->signal->sig_pending &= ~(1UL << (sig - 1));

	/* SIGKILL cannot be caught. Global init is protected above SIG_DFL. */
	if (sig == SIGKILL) {
		if (cur->psid == 1) {
			maybe_restore_sigmask(cur);
			return;
		}
		do_group_exit(sig);
		return;
	}

	sa = &cur->signal->sig_handlers[sig];

	if (sa->sa_handler == SIG_IGN) {
		maybe_restore_sigmask(cur);
		return;
	}

	if (sa->sa_handler == SIG_DFL) {
		handle_sig_dfl(cur, frame, sig);
		goto next_signal;
	}

	arch_signal_deliver(cur, frame, sa, sig, NULL);
}

/* Synchronous faults deliver their own context before unrelated pending signals. */
void ps_fault_signal(intr_frame *frame, int sig,
		     const struct signal_fault *fault)
{
	task_struct *cur = CURRENT_TASK();
	struct sigaction *sa = &cur->signal->sig_handlers[sig];
	unsigned long bit = 1UL << (sig - 1);
	if (!arch_interrupt_frame_is_user(frame)) {
		do_group_exit(sig);
		return;
	}
	if ((cur->signal->sig_mask & bit) || sa->sa_handler == SIG_IGN) {
		cur->signal->sig_mask &= ~bit;
		sa->sa_handler = SIG_DFL;
	}
	cur->signal->sig_pending &= ~bit;
	if (sa->sa_handler == SIG_DFL) {
		do_group_exit(sig);
		return;
	}
	arch_signal_deliver(cur, frame, sa, sig, fault);
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

	if (timeout && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
			timeout->tv_nsec >= 1000000000))
		return -EINVAL;

	wait_set = *set;
	wait_set &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));

	if (timeout) {
		deadline = time_deadline_ms(
			(unsigned long long)timeout->tv_sec * 1000 +
			(timeout->tv_nsec + 999999ULL) / 1000000);
		has_timeout = 1;
	}

	for (;;) {
		unsigned sleep_ms = 0;
		sigset_t pending = cur->signal->sig_pending & wait_set;
		if (pending & (1UL << (SIGRTMIN_KERNEL - 1))) {
			cur->signal->sig_pending &=
				~(1UL << (SIGRTMIN_KERNEL - 1));
			if (info) {
				uint32_t *words = info;
				memset(info, 0, 128);
				words[0] = SIGRTMIN_KERNEL;
				words[2] = (uint32_t)-2;
				words[3] = cur->signal->timer_signal_id;
				words[5] = cur->signal->timer_signal_value;
			}
			return SIGRTMIN_KERNEL;
		}

		if (pending) {
			int sig = __builtin_ctz((unsigned)pending) + 1;
			cur->signal->sig_pending &= ~(1U << (sig - 1));
			if (info)
				memset(info, 0, sigsetsize);
			return sig;
		}

		/* A non-waited, unblocked signal interrupts the wait. */
		if (ps_interrupting_signals(cur) & ~wait_set)
			return -EINTR;

		if (has_timeout) {
			unsigned long long now = time_now_ms();

			if (now >= deadline)
				return -EAGAIN;
			sleep_ms = deadline - now > 0xffffffffULL ?
					   0xffffffffU :
					   (unsigned)(deadline - now);
		}
		/* Explicitly awaited signals may be blocked in sig_mask. */
		cur->signal_wait_mask = wait_set;
		if (!ps_prepare_interruptible_wait(cur, NULL, sleep_ms,
						   __func__)) {
			task_sched();
			ps_finish_timed_wait(cur);
		}
		cur->signal_wait_mask = 0;
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
