#include <ps/ps.h>
#include <ps/task.h>
#include <ps/signal.h>
#include <int/int.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <errno.h>

#include "ps_internal.h"
#include <ps/ptrace.h>

#define W_STOPCODE(sig) (((sig) << 8) | 0x7f)

static int ptrace_is_traced_by(task_struct *target, task_struct *tracer)
{
	return target && tracer && target->user &&
	       target->user->ptrace_tracer == tracer->psid;
}

static void ptrace_notify_parent_unsafe(task_struct *task)
{
	task_struct *parent;

	if (!task->ppid)
		return;

	parent = ps_find_process_unsafe(task->ppid);
	if (!parent || !parent->signal)
		return;

	parent->signal->sig_pending |= (1UL << (SIGCHLD - 1));
	ps_put_to_ready_queue_unsafe(parent);
}

static void ptrace_stop_task_unsafe(task_struct *task, int sig,
				    intr_frame *frame, const char *func)
{
	list_remove_entry(&task->ps_list);
	if (task->psid != 0xffffffff)
		list_insert_tail(&control.wait_queue, &task->ps_list);
	task->status = ps_stopped;
	task->wait_func = func;
	task->stop_signal = sig;
	task->stop_report_pending = 1;
	if (frame) {
		arch_ptrace_save(task, frame);
		task->user->ptrace_frame_valid = 1;
	} else {
		memset(&task->user->ptrace_frame, 0,
		       sizeof(task->user->ptrace_frame));
		task->user->ptrace_frame_valid = 0;
	}
	ptrace_notify_parent_unsafe(task);
}

static int ptrace_resume(task_struct *tracer, task_struct *target, int mode,
			 int sig)
{
	int irq;

	if (!ptrace_is_traced_by(target, tracer) ||
	    target->status != ps_stopped)
		return -ESRCH;

	if (sig < 0 || sig >= NSIG)
		return -EINVAL;

	if (sig > 0 && target->signal)
		target->signal->sig_pending |= (1UL << (sig - 1));

	spinlock_lock(&ps_lock, &irq);
	target->user->ptrace_mode = mode;
	target->user->ptrace_frame_valid = 0;
	memset(&target->user->ptrace_frame, 0,
	       sizeof(target->user->ptrace_frame));
	target->stop_signal = 0;
	target->stop_report_pending = 0;
	ps_put_to_ready_queue_unsafe(target);
	spinlock_unlock(&ps_lock, irq);
	return 0;
}

void ps_stop_current(intr_frame *frame, int sig)
{
	task_struct *cur = CURRENT_TASK();
	int irq;

	spinlock_lock(&ps_lock, &irq);
	ptrace_stop_task_unsafe(cur, sig, frame, __func__);
	spinlock_unlock(&ps_lock, irq);
	task_sched();
}

int ps_ptrace_maybe_stop_syscall(intr_frame *frame, int entering)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame stop_frame;
	intr_frame *saved_frame = frame;
	int irq;

	if (!cur->user || !cur->user->ptrace_tracer ||
	    cur->user->ptrace_mode != PTRACE_MODE_SYSCALL)
		return 0;

	if (entering) {
		cur->user->ptrace_orig_eax = frame->eax;
		/*
		 * Linux reports EAX as -ENOSYS at syscall-entry ptrace
		 * stops on i386. strace uses ORIG_EAX for the syscall
		 * number and this sentinel in EAX to distinguish entry
		 * from a stray exit.
		 */
		stop_frame = *frame;
		stop_frame.eax = -ENOSYS;
		saved_frame = &stop_frame;
	}

	spinlock_lock(&ps_lock, &irq);
	ptrace_stop_task_unsafe(
		cur,
		SIGTRAP | (cur->user->ptrace_options & PTRACE_O_TRACESYSGOOD ?
				   0x80 :
				   0),
		saved_frame, entering ? "ptrace-sys-enter" : "ptrace-sys-exit");
	spinlock_unlock(&ps_lock, irq);
	task_sched();
	return 1;
}

void ps_ptrace_stop_exec(vaddr_t eip, vaddr_t esp, unsigned syscall_number)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame frame;
	int irq;

	if (!cur->user->ptrace_tracer)
		return;

	arch_task_init_user_frame(&frame, eip, esp);
	frame.eax = 0;
	cur->user->ptrace_orig_eax = syscall_number;

	spinlock_lock(&ps_lock, &irq);
	cur->user->ptrace_eventmsg = cur->psid;
	ptrace_stop_task_unsafe(cur,
				SIGTRAP | (cur->user->ptrace_options &
							   PTRACE_O_TRACEEXEC ?
						   PTRACE_EVENT_EXEC << 8 :
						   0),
				&frame, "ptrace-exec");
	spinlock_unlock(&ps_lock, irq);
	task_sched();

	/* Successful exec switches to userspace without returning to the dispatcher. */
	ps_ptrace_maybe_stop_syscall(&frame, 0);
}

void ps_ptrace_stop_exit(unsigned status)
{
	task_struct *cur = CURRENT_TASK();
	int irq;

	if (!cur->user || !cur->user->ptrace_tracer ||
	    !(cur->user->ptrace_options & PTRACE_O_TRACEEXIT))
		return;
	spinlock_lock(&ps_lock, &irq);
	cur->user->ptrace_eventmsg = status;
	ptrace_stop_task_unsafe(cur, SIGTRAP | (PTRACE_EVENT_EXIT << 8), NULL,
				"ptrace-exit");
	spinlock_unlock(&ps_lock, irq);
	task_sched();
}

int ps_ptrace_control(int request, int pid, void *addr, void *data)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *target;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("ptrace(%d, %d, %x, %x)\n", request, pid, addr, data);

	switch (request) {
	case PTRACE_TRACEME:
		if (cur->user->ptrace_tracer)
			return -EPERM;
		cur->user->ptrace_tracer = cur->ppid;
		cur->user->ptrace_mode = PTRACE_MODE_NONE;
		cur->user->ptrace_options = 0;
		cur->user->ptrace_eventmsg = 0;
		cur->user->ptrace_orig_eax = 0;
		return 0;

	case PTRACE_SEIZE:
		return -EIO;
	case PTRACE_ATTACH:
	case PTRACE_DETACH:
		return -ENOSYS;
	}

	target = ps_find_process((unsigned)(uintptr_t)pid);
	if (!target)
		return -ESRCH;

	if (!ptrace_is_traced_by(target, cur))
		return -EPERM;

	switch (request) {
	case PTRACE_SETOPTIONS:
		if (target->status != ps_stopped)
			return -ESRCH;
		if ((uintptr_t)data &
		    ~(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC |
		      PTRACE_O_TRACEEXIT))
			return -EINVAL;
		target->user->ptrace_options = (uintptr_t)data;
		return 0;

	case PTRACE_CONT:
		return ptrace_resume(cur, target, PTRACE_MODE_CONT,
				     (int)(unsigned long)data);

	case PTRACE_SYSCALL:
		return ptrace_resume(cur, target, PTRACE_MODE_SYSCALL,
				     (int)(unsigned long)data);

	case PTRACE_KILL:
		if (target->status != ps_stopped)
			return -ESRCH;
		if (target->signal)
			target->signal->sig_pending |= (1UL << (SIGKILL - 1));
		return ptrace_resume(cur, target, PTRACE_MODE_CONT, 0);

	default:
		return -EINVAL;
	}
}

int ps_ptrace_target(int pid, task_struct **result)
{
	task_struct *target = ps_find_process(pid);
	if (!target)
		return -ESRCH;
	if (!ptrace_is_traced_by(target, current))
		return -EPERM;
	*result = target;
	return 0;
}
