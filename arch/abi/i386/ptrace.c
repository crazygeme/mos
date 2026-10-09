#include <ps/ptrace.h>
#include <lib/klib.h>
#include <errno.h>
struct ptrace_user_regs {
	uint32_t ebx, ecx, edx, esi, edi, ebp, eax;
	uint32_t xds, xes, xfs, xgs, orig_eax, eip, xcs, eflags, esp, xss;
};
static int i386_copy_regs(task_struct *task, struct ptrace_user_regs *regs)
{
	ptrace_saved_frame *frame = &task->execution->ptrace_frame;

	if (!task->execution->ptrace_frame_valid)
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
	regs->orig_eax = task->execution->ptrace_orig_eax;
	regs->eip = frame->eip;
	regs->xcs = frame->cs;
	regs->eflags = frame->eflags;
	regs->esp = frame->esp;
	regs->xss = frame->ss;
	return 0;
}

#define PTRACE_WORD uint32_t
#define PTRACE_REGS struct ptrace_user_regs
#define PTRACE_PREFIX(name) i386_##name
#include <arch/abi/ptrace.h>
