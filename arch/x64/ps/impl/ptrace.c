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
	task->execution->ptrace_frame.r8 = frame->r8;
	task->execution->ptrace_frame.r9 = frame->r9;
	task->execution->ptrace_frame.r10 = frame->r10;
	task->execution->ptrace_frame.r11 = frame->r11;
	task->execution->ptrace_frame.r12 = frame->r12;
	task->execution->ptrace_frame.r13 = frame->r13;
	task->execution->ptrace_frame.r14 = frame->r14;
	task->execution->ptrace_frame.r15 = frame->r15;
	task->execution->ptrace_frame.fs_base = task->execution->arch.fs_base;
	task->execution->ptrace_frame.gs_base = task->execution->arch.gs_base;
}
