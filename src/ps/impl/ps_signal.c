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
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <int/interrupt.h>
#include "ps_internal.h"

void ps_signal_action(task_struct *task, int sig, const struct sigaction *in,
		      struct sigaction *out)
{
	int irq;
	spinlock_lock(&ps_lock, &irq);
	struct sigaction *action = &task->sighand->actions[sig];
	if (out)
		*out = *action;
	if (in)
		*action = *in;
	spinlock_unlock(&ps_lock, irq);
}

void ps_claim_signal_action(task_struct *task, int sig, struct sigaction *out)
{
	int irq;
	spinlock_lock(&ps_lock, &irq);
	*out = task->sighand->actions[sig];
	if (out->sa_flags & SA_RESETHAND)
		task->sighand->actions[sig].sa_handler = SIG_DFL;
	spinlock_unlock(&ps_lock, irq);
}

unsigned long ps_pending_signals(task_struct *task)
{
	unsigned long pending =
		task->sighand ? __atomic_load_n(&task->signal->sig_pending,
						__ATOMIC_ACQUIRE) :
				0;
	if (task->execution && task->thread)
		pending |= __atomic_load_n(&task->thread->sig_pending,
					   __ATOMIC_ACQUIRE);
	return pending;
}

static void wake_signal_target(task_struct *task, int sig)
{
	unsigned long bit = 1UL << (sig - 1);
	if (task->sched->status == ps_stopped &&
	    (sig == SIGCONT || sig == SIGKILL)) {
		task->life->stop_signal = task->life->stop_report_pending = 0;
		ps_put_to_ready_queue_unsafe(task);
	} else if (task->sched->status == ps_waiting &&
		   task->wait->wait_interruptible &&
		   (ps_interrupting_signals(task) ||
		    (task->wait->signal_wait_mask & bit)))
		ps_put_to_ready_queue_unsafe(task);
}

/* Process pending state remains available to every eligible thread. */
void ps_queue_group_signal_unsafe(task_struct *task, int sig)
{
	task_thread *group = task->thread;
	unsigned long bit = 1UL << (sig - 1);
	if (sig == SIGCONT)
		__atomic_fetch_and(
			&group->sig_pending,
			~((1UL << (SIGSTOP - 1)) | (1UL << (SIGTSTP - 1)) |
			  (1UL << (SIGTTIN - 1)) | (1UL << (SIGTTOU - 1))),
			__ATOMIC_RELEASE);
	else if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
		 sig == SIGTTOU)
		__atomic_fetch_and(&group->sig_pending, ~(1UL << (SIGCONT - 1)),
				   __ATOMIC_RELEASE);
	__atomic_fetch_or(&group->sig_pending, bit, __ATOMIC_RELEASE);
	for (struct rb_node *node = rb_first(&control.mgr_queue); node;
	     node = rb_next(node)) {
		task_struct *peer =
			(rb_entry(node, task_schedule, mgr_rb)->task);
		if (peer->execution && peer->thread == group && peer->sighand &&
		    peer->sched->status != ps_dying)
			wake_signal_target(peer, sig);
	}
}

void ps_timer_notify(unsigned tid, int signo, int timer_id, uintptr_t value,
		     int thread)
{
	signal_queue_entry *entry = kmalloc(sizeof(*entry));
	if (!entry)
		return;
	*entry = (signal_queue_entry){ .signo = signo,
				       .timer_id = timer_id,
				       .value = value };
	int irq;
	spinlock_lock(&ps_lock, &irq);
	task_struct *target = ps_find_process_unsafe(tid);
	if (target && target->life->type == ps_user && target->sighand &&
	    target->sched->status != ps_dying) {
		list_entry *queue = thread ? &target->signal->pending_queue :
					     &target->thread->pending_queue;
		for (list_entry *node = queue->next; node != queue;
		     node = node->next) {
			signal_queue_entry *pending =
				container_of(node, signal_queue_entry, list);
			if (pending->timer_id == timer_id &&
			    pending->signo == signo) {
				kfree(entry);
				spinlock_unlock(&ps_lock, irq);
				return;
			}
		}
		list_insert_tail(queue, &entry->list);
		if (thread)
			ps_queue_signal_unsafe(target, signo);
		else
			ps_queue_group_signal_unsafe(target, signo);
	} else
		kfree(entry);
	spinlock_unlock(&ps_lock, irq);
}

/* Consume bit and payload together under ps_lock. Keep the bit set while
 * another event for that signal is queued. Thread-directed events win ties. */
int ps_take_signal(task_struct *task, sigset_t mask, int *timer_id,
		   uintptr_t *value)
{
	int irq, sig = 0;
	spinlock_lock(&ps_lock, &irq);
	sigset_t pending = ps_pending_signals(task) & mask;
	if (pending) {
		sig = __builtin_ctzl(pending) + 1;
		unsigned long bit = 1UL << (sig - 1);
		int local = !!(task->signal->sig_pending & bit);
		sigset_t *bits = local ? &task->signal->sig_pending :
					 &task->thread->sig_pending;
		list_entry *queue = local ? &task->signal->pending_queue :
					    &task->thread->pending_queue;
		if (timer_id)
			*timer_id = -1;
		if (value)
			*value = 0;
		for (list_entry *node = queue->next; node != queue;
		     node = node->next) {
			signal_queue_entry *entry =
				container_of(node, signal_queue_entry, list);
			if (entry->signo != sig)
				continue;
			if (timer_id)
				*timer_id = entry->timer_id;
			if (value)
				*value = entry->value;
			list_remove_entry(node);
			kfree(entry);
			break;
		}
		int more = 0;
		for (list_entry *node = queue->next; node != queue;
		     node = node->next)
			more |= container_of(node, signal_queue_entry, list)
					->signo == sig;
		if (!more)
			__atomic_fetch_and(bits, ~bit, __ATOMIC_RELEASE);
	}
	spinlock_unlock(&ps_lock, irq);
	return sig;
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
	task_credentials *su, *tu;

	if (!sender || !target || !target->execution)
		return 0;

	su = sender->credentials;
	tu = target->credentials;
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

	if (task->life->type != ps_user ||
	    task->life->psid != task->thread->tgid ||
	    task->life->psid == c->self->life->psid)
		return;
	if (ps_send_signal(task->life->psid, c->sig) == 0)
		c->sent++;
}

static void send_if_pgrp(task_struct *task, void *opaque)
{
	struct kill_all_ctx *c = opaque;

	if (task->life->type != ps_user || !task->execution ||
	    task->life->psid != task->thread->tgid ||
	    task->life->psid == c->self->life->psid)
		return;
	if (task->thread->group_id != c->pgrp)
		return;
	if (ps_send_signal(task->life->psid, c->sig) == 0)
		c->sent++;
}

unsigned long ps_interrupting_signals(task_struct *task)
{
	task_signal *signal = task->signal;
	unsigned long pending;
	int sig;

	if (!task->sighand)
		return 0;
	pending = ps_pending_signals(task) & ~signal->sig_mask;
	if (!pending)
		return 0;
	for (sig = 1; sig < NSIG; sig++) {
		void (*handler)(int);
		unsigned long bit = 1UL << (sig - 1);

		if (!(pending & bit))
			continue;
		handler =
			__atomic_load_n(&task->sighand->actions[sig].sa_handler,
					__ATOMIC_RELAXED);
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
		__atomic_fetch_and(&target->signal->sig_pending,
				   ~STOP_SIGNALS_MASK, __ATOMIC_RELEASE);
	else if (bit & STOP_SIGNALS_MASK)
		__atomic_fetch_and(&target->signal->sig_pending,
				   ~(1UL << (SIGCONT - 1)), __ATOMIC_RELEASE);

	__atomic_fetch_or(&target->signal->sig_pending, bit, __ATOMIC_RELEASE);
	wake_signal_target(target, sig);
}

/* Check sender permissions, queue the signal, and wake eligible recipients. */
static int send_signal(unsigned pid, int sig, int thread)
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

	if (target->life->type != ps_user || !target->execution ||
	    !target->sighand) {
		ret = -ESRCH;
		goto done;
	}

	if (!can_send_signal(sender, target)) {
		ret = -EPERM;
		goto done;
	}

	if (sig) {
		if (thread)
			ps_queue_signal_unsafe(target, sig);
		else
			ps_queue_group_signal_unsafe(target, sig);
	}

done:
	spinlock_unlock(&ps_lock, irq);
	return ret;
}

int ps_send_signal(unsigned pid, int sig)
{
	return send_signal(pid, sig, 0);
}
int ps_send_thread_signal(unsigned tid, int sig)
{
	return send_signal(tid, sig, 1);
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
		if (ret == 0 && cur->life->type == ps_user &&
		    (unsigned)(uintptr_t)pid == cur->life->psid &&
		    cur->sighand &&
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
		if (!cur->execution)
			return -ESRCH;
		ctx.self = cur;
		ctx.sig = sig;
		ctx.pgrp = cur->thread->group_id;
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

	if (sig <= 0 || sig >= NSIG)
		return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP)
		return -EINVAL;

	struct sigaction input, output;
	if (act)
		input = *(struct sigaction *)act;
	ps_signal_action(cur, sig, act ? &input : NULL, oact ? &output : NULL);
	if (oact)
		*(struct sigaction *)oact = output;

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
static int pick_signal(task_struct *cur, int *timer_id, uintptr_t *value)
{
	unsigned long ignored = ps_pending_signals(cur) &
				~cur->signal->sig_mask &
				~ps_interrupting_signals(cur);
	while (ignored) {
		int sig = ps_take_signal(cur, ignored, NULL, NULL);
		if (!sig)
			break;
		ignored = ps_pending_signals(cur) & ~cur->signal->sig_mask &
			  ~ps_interrupting_signals(cur);
	}
	return ps_take_signal(cur, ps_interrupting_signals(cur), timer_id,
			      value);
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
	if (cur->life->psid == 1) {
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

	if (cur->life->type != ps_user)
		return;
	if (!arch_interrupt_frame_is_user(frame))
		return;

next_signal:
	int timer_id;
	uintptr_t timer_value;
	sig = pick_signal(cur, &timer_id, &timer_value);
	if (!sig)
		return;

	/* SIGKILL cannot be caught. Global init is protected above SIG_DFL. */
	if (sig == SIGKILL) {
		if (cur->life->psid == 1) {
			maybe_restore_sigmask(cur);
			return;
		}
		do_group_exit(sig);
		return;
	}

	struct sigaction action;
	ps_claim_signal_action(cur, sig, &action);
	sa = &action;

	if (sa->sa_handler == SIG_IGN) {
		maybe_restore_sigmask(cur);
		return;
	}

	if (sa->sa_handler == SIG_DFL) {
		handle_sig_dfl(cur, frame, sig);
		goto next_signal;
	}

	struct signal_fault event = { .code = -2,
				      .timer_id = timer_id,
				      .value = timer_value };
	arch_signal_deliver(cur, frame, sa, sig, timer_id >= 0 ? &event : NULL);
}

/* Synchronous faults deliver their own context before unrelated pending signals. */
void ps_fault_signal(intr_frame *frame, int sig,
		     const struct signal_fault *fault)
{
	task_struct *cur = CURRENT_TASK();
	struct sigaction action;
	ps_claim_signal_action(cur, sig, &action);
	struct sigaction *sa = &action;
	unsigned long bit = 1UL << (sig - 1);
	if (!arch_interrupt_frame_is_user(frame)) {
		do_group_exit(sig);
		return;
	}
	if ((cur->signal->sig_mask & bit) || sa->sa_handler == SIG_IGN) {
		cur->signal->sig_mask &= ~bit;
		sa->sa_handler = SIG_DFL;
		ps_signal_action(cur, sig, sa, NULL);
	}
	__atomic_fetch_and(&cur->signal->sig_pending, ~bit, __ATOMIC_RELEASE);
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

	*set = ps_pending_signals(cur) & cur->signal->sig_mask;
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
		int timer_id;
		uintptr_t value;
		int sig = ps_take_signal(cur, wait_set, &timer_id, &value);
		if (sig) {
			if (info) {
				uint32_t *words = info;
				memset(info, 0, 128);
				words[0] = sig;
				if (timer_id >= 0) {
					words[2] = (uint32_t)-2;
					words[3] = timer_id;
					words[5] = (uint32_t)value;
#if defined(__x86_64__)
					words[6] = (uint32_t)(value >> 32);
#endif
				}
			}
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
		cur->wait->signal_wait_mask = wait_set;
		if (!ps_prepare_interruptible_wait(cur, NULL, sleep_ms)) {
			task_sched();
			ps_finish_timed_wait(cur);
		}
		cur->wait->signal_wait_mask = 0;
	}
}

/*
 * rt_sigqueueinfo (178) — send a signal with siginfo to a group.
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
