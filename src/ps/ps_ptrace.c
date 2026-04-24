#include <ps/ps.h>
#include <ps/signal.h>
#include <int/int.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <errno.h>

#include "ps_internal.h"

#define PTRACE_TRACEME 0
#define PTRACE_PEEKTEXT 1
#define PTRACE_PEEKDATA 2
#define PTRACE_PEEKUSER 3
#define PTRACE_CONT 7
#define PTRACE_KILL 8
#define PTRACE_GETREGS 12
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
#define PTRACE_SYSCALL 24

#define PTRACE_MODE_NONE 0
#define PTRACE_MODE_CONT 1
#define PTRACE_MODE_SYSCALL 2

#define W_STOPCODE(sig) (((sig) << 8) | 0x7f)

enum ptrace_user_reg_index {
	PTRACE_REG_EBX = 0,
	PTRACE_REG_ECX,
	PTRACE_REG_EDX,
	PTRACE_REG_ESI,
	PTRACE_REG_EDI,
	PTRACE_REG_EBP,
	PTRACE_REG_EAX,
	PTRACE_REG_DS,
	PTRACE_REG_ES,
	PTRACE_REG_FS,
	PTRACE_REG_GS,
	PTRACE_REG_ORIG_EAX,
	PTRACE_REG_EIP,
	PTRACE_REG_CS,
	PTRACE_REG_EFL,
	PTRACE_REG_UESP,
	PTRACE_REG_SS,
};

struct ptrace_user_regs {
	unsigned ebx;
	unsigned ecx;
	unsigned edx;
	unsigned esi;
	unsigned edi;
	unsigned ebp;
	unsigned eax;
	unsigned xds;
	unsigned xes;
	unsigned xfs;
	unsigned xgs;
	unsigned orig_eax;
	unsigned eip;
	unsigned xcs;
	unsigned eflags;
	unsigned esp;
	unsigned xss;
};

static int ptrace_is_traced_by(task_struct *target, task_struct *tracer)
{
	return target && tracer && target->ptrace_tracer == tracer->psid;
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
	if (parent->status == ps_waiting)
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
		task->ptrace_frame.edi = frame->edi;
		task->ptrace_frame.esi = frame->esi;
		task->ptrace_frame.ebp = frame->ebp;
		task->ptrace_frame.ebx = frame->ebx;
		task->ptrace_frame.edx = frame->edx;
		task->ptrace_frame.ecx = frame->ecx;
		task->ptrace_frame.eax = frame->eax;
		task->ptrace_frame.gs = frame->gs;
		task->ptrace_frame.fs = frame->fs;
		task->ptrace_frame.es = frame->es;
		task->ptrace_frame.ds = frame->ds;
		task->ptrace_frame.error_code = frame->error_code;
		task->ptrace_frame.eip = (unsigned)frame->eip;
		task->ptrace_frame.cs = frame->cs;
		task->ptrace_frame.eflags = frame->eflags;
		task->ptrace_frame.esp = (unsigned)frame->esp;
		task->ptrace_frame.ss = frame->ss;
		task->ptrace_frame_valid = 1;
	} else {
		memset(&task->ptrace_frame, 0, sizeof(task->ptrace_frame));
		task->ptrace_frame_valid = 0;
	}
	ptrace_notify_parent_unsafe(task);
}

static int ptrace_copy_regs(task_struct *task, struct ptrace_user_regs *regs)
{
	ptrace_saved_frame *frame = &task->ptrace_frame;

	if (!task->ptrace_frame_valid)
		return -EIO;

	memset(regs, 0, sizeof(*regs));
	regs->ebx = frame->ebx;
	regs->ecx = frame->ecx;
	regs->edx = frame->edx;
	regs->esi = frame->esi;
	regs->edi = frame->edi;
	regs->ebp = frame->ebp;
	regs->eax = frame->eax;
	regs->xds = frame->ds;
	regs->xes = frame->es;
	regs->xfs = frame->fs;
	regs->xgs = frame->gs;
	regs->orig_eax = task->ptrace_orig_eax;
	regs->eip = (unsigned)frame->eip;
	regs->xcs = frame->cs;
	regs->eflags = frame->eflags;
	regs->esp = (unsigned)frame->esp;
	regs->xss = frame->ss;
	return 0;
}

static int ptrace_peekuser(task_struct *task, unsigned addr, long *out)
{
	ptrace_saved_frame *frame = &task->ptrace_frame;
	unsigned index = addr / sizeof(unsigned);

	if (!task->ptrace_frame_valid || (addr & (sizeof(unsigned) - 1)))
		return -EIO;

	switch (index) {
	case PTRACE_REG_EBX:
		*out = frame->ebx;
		return 0;
	case PTRACE_REG_ECX:
		*out = frame->ecx;
		return 0;
	case PTRACE_REG_EDX:
		*out = frame->edx;
		return 0;
	case PTRACE_REG_ESI:
		*out = frame->esi;
		return 0;
	case PTRACE_REG_EDI:
		*out = frame->edi;
		return 0;
	case PTRACE_REG_EBP:
		*out = frame->ebp;
		return 0;
	case PTRACE_REG_EAX:
		*out = frame->eax;
		return 0;
	case PTRACE_REG_DS:
		*out = frame->ds;
		return 0;
	case PTRACE_REG_ES:
		*out = frame->es;
		return 0;
	case PTRACE_REG_FS:
		*out = frame->fs;
		return 0;
	case PTRACE_REG_GS:
		*out = frame->gs;
		return 0;
	case PTRACE_REG_ORIG_EAX:
		*out = task->ptrace_orig_eax;
		return 0;
	case PTRACE_REG_EIP:
		*out = (unsigned)frame->eip;
		return 0;
	case PTRACE_REG_CS:
		*out = frame->cs;
		return 0;
	case PTRACE_REG_EFL:
		*out = frame->eflags;
		return 0;
	case PTRACE_REG_UESP:
		*out = (unsigned)frame->esp;
		return 0;
	case PTRACE_REG_SS:
		*out = frame->ss;
		return 0;
	default:
		return -EIO;
	}
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
	target->ptrace_mode = mode;
	target->ptrace_frame_valid = 0;
	memset(&target->ptrace_frame, 0, sizeof(target->ptrace_frame));
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

void ps_ptrace_maybe_stop_syscall(intr_frame *frame, int entering)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame stop_frame;
	intr_frame *saved_frame = frame;
	int irq;

	if (!cur->ptrace_tracer || cur->ptrace_mode != PTRACE_MODE_SYSCALL)
		return;

	if (entering) {
		cur->ptrace_orig_eax = frame->eax;
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
	ptrace_stop_task_unsafe(cur, SIGTRAP, saved_frame,
				entering ? "ptrace-sys-enter" :
					   "ptrace-sys-exit");
	spinlock_unlock(&ps_lock, irq);
	task_sched();
}

void ps_ptrace_stop_exec(unsigned eip, unsigned esp)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame frame;
	int irq;

	if (!cur->ptrace_tracer)
		return;

	memset(&frame, 0, sizeof(frame));
	frame.eip = (void *)eip;
	frame.esp = (void *)esp;
	frame.cs = USER_CODE_SELECTOR;
	frame.ss = USER_DATA_SELECTOR;
	frame.ds = USER_DATA_SELECTOR;
	frame.es = USER_DATA_SELECTOR;
	frame.fs = USER_DATA_SELECTOR;
	frame.gs = USER_DATA_SELECTOR;
	frame.eflags = 0x202;
	frame.eax = 0;
	cur->ptrace_orig_eax = 11; /* __NR_execve on i386 */

	spinlock_lock(&ps_lock, &irq);
	ptrace_stop_task_unsafe(cur, SIGTRAP, &frame, "ptrace-exec");
	spinlock_unlock(&ps_lock, irq);
	task_sched();
}

int sys_ptrace(int request, int pid, void *addr, void *data)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *target;
	struct ptrace_user_regs regs;
	long peek_word;
	union {
		long word;
		char bytes[sizeof(long)];
	} peek;
	int ret;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("ptrace(%d, %d, %x, %x)\n", request, pid, addr, data);

	switch (request) {
	case PTRACE_TRACEME:
		if (cur->ptrace_tracer)
			return -EPERM;
		cur->ptrace_tracer = cur->ppid;
		cur->ptrace_mode = PTRACE_MODE_NONE;
		cur->ptrace_orig_eax = 0;
		return 0;

	case PTRACE_ATTACH:
	case PTRACE_DETACH:
		return -ENOSYS;
	}

	target = ps_find_process((unsigned)pid);
	if (!target)
		return -ESRCH;

	if (!ptrace_is_traced_by(target, cur))
		return -EPERM;

	switch (request) {
	case PTRACE_PEEKDATA:
	case PTRACE_PEEKTEXT:
		memset(&peek, 0, sizeof(peek));
		ret = ps_read_process_memory(target, addr, peek.bytes,
					     sizeof(peek.bytes));
		if (ret < 0)
			return ret;
		*(long *)data = peek.word;
		return 0;

	case PTRACE_PEEKUSER:
		ret = ptrace_peekuser(target, (unsigned)addr, &peek_word);
		if (ret < 0)
			return ret;
		*(long *)data = peek_word;
		return 0;

	case PTRACE_GETREGS:
		ret = ptrace_copy_regs(target, &regs);
		if (ret < 0)
			return ret;
		memcpy(data, &regs, sizeof(regs));
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
