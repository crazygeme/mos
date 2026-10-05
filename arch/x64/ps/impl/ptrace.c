#include <ps/ptrace.h>
void arch_ptrace_save(task_struct *task, intr_frame *frame)
{
	task->user->ptrace_frame.edi = frame->edi;
	task->user->ptrace_frame.esi = frame->esi;
	task->user->ptrace_frame.ebp = frame->ebp;
	task->user->ptrace_frame.ebx = frame->ebx;
	task->user->ptrace_frame.edx = frame->edx;
	task->user->ptrace_frame.ecx = frame->ecx;
	task->user->ptrace_frame.eax = frame->eax;
	task->user->ptrace_frame.gs = frame->gs;
	task->user->ptrace_frame.fs = frame->fs;
	task->user->ptrace_frame.es = frame->es;
	task->user->ptrace_frame.ds = frame->ds;
	task->user->ptrace_frame.error_code = frame->error_code;
	task->user->ptrace_frame.eip = (uintptr_t)frame->eip;
	task->user->ptrace_frame.cs = frame->cs;
	task->user->ptrace_frame.eflags = frame->eflags;
	task->user->ptrace_frame.esp = (uintptr_t)frame->esp;
	task->user->ptrace_frame.ss = frame->ss;
	task->user->ptrace_frame.r8 = frame->r8;
	task->user->ptrace_frame.r9 = frame->r9;
	task->user->ptrace_frame.r10 = frame->r10;
	task->user->ptrace_frame.r11 = frame->r11;
	task->user->ptrace_frame.r12 = frame->r12;
	task->user->ptrace_frame.r13 = frame->r13;
	task->user->ptrace_frame.r14 = frame->r14;
	task->user->ptrace_frame.r15 = frame->r15;
	task->user->ptrace_frame.fs_base = task->tss.fs_base;
	task->user->ptrace_frame.gs_base = task->tss.gs_base;
}
