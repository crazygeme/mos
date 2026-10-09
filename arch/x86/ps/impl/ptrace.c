#include <ps/ptrace.h>
void arch_ptrace_save(task_struct *task, intr_frame *frame)
{
	task->execution->ptrace_frame.edi = frame->edi;
	task->execution->ptrace_frame.esi = frame->esi;
	task->execution->ptrace_frame.ebp = frame->ebp;
	task->execution->ptrace_frame.ebx = frame->ebx;
	task->execution->ptrace_frame.edx = frame->edx;
	task->execution->ptrace_frame.ecx = frame->ecx;
	task->execution->ptrace_frame.eax = frame->eax;
	task->execution->ptrace_frame.gs = frame->gs;
	task->execution->ptrace_frame.fs = frame->fs;
	task->execution->ptrace_frame.es = frame->es;
	task->execution->ptrace_frame.ds = frame->ds;
	task->execution->ptrace_frame.error_code = frame->error_code;
	task->execution->ptrace_frame.eip = (uintptr_t)frame->eip;
	task->execution->ptrace_frame.cs = frame->cs;
	task->execution->ptrace_frame.eflags = frame->eflags;
	task->execution->ptrace_frame.esp = (uintptr_t)frame->esp;
	task->execution->ptrace_frame.ss = frame->ss;
}
