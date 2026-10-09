#include <ps/ptrace.h>
#include <lib/klib.h>
#include <errno.h>
typedef uint64_t native_regs[27];
static int native_copy_regs(task_struct *task, native_regs *output)
{
	ptrace_saved_frame *f = &task->execution->ptrace_frame;
	if (!task->execution->ptrace_frame_valid)
		return -EIO;
	const uint64_t regs[27] = {
		f->r15,	    f->r14,
		f->r13,	    f->r12,
		f->ebp,	    f->ebx,
		f->r11,	    f->r10,
		f->r9,	    f->r8,
		f->eax,	    f->ecx,
		f->edx,	    f->esi,
		f->edi,	    task->execution->ptrace_orig_eax,
		f->eip,	    f->cs,
		f->eflags,  f->esp,
		f->ss,	    f->fs_base,
		f->gs_base, f->ds,
		f->es,	    f->fs,
		f->gs,
	};
	memcpy(output, regs, sizeof(regs));
	return 0;
}
#define PTRACE_WORD uint64_t
#define PTRACE_REGS native_regs
#define PTRACE_PREFIX(name) native_##name
#include <arch/abi/ptrace.h>
