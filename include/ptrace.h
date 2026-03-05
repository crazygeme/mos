#ifndef _PTRACE_H_
#define _PTRACE_H_

#include <int.h>
#include <config.h>

/* ptrace request codes (Linux i386 compatible) */
#define PTRACE_TRACEME 0 /* child requests to be traced by parent */
#define PTRACE_PEEKTEXT 1 /* read word from tracee text */
#define PTRACE_PEEKDATA 2 /* read word from tracee data */
#define PTRACE_PEEKUSER 3 /* read word from tracee user area (registers) */
#define PTRACE_POKETEXT 4 /* write word to tracee text */
#define PTRACE_POKEDATA 5 /* write word to tracee data */
#define PTRACE_POKEUSER 6 /* write word to tracee user area (registers) */
#define PTRACE_CONT 7 /* resume execution */
#define PTRACE_KILL 8 /* kill the tracee */
#define PTRACE_SINGLESTEP 9 /* execute one instruction */
#define PTRACE_GETREGS 12 /* get all general-purpose registers */
#define PTRACE_SETREGS 13 /* set all general-purpose registers */
#define PTRACE_ATTACH 16 /* attach to a running process */
#define PTRACE_DETACH 17 /* detach and resume */
#define PTRACE_SYSCALL 24 /* resume, stop at next syscall entry/exit */

/*
 * Flags stored in task_struct.ptrace.
 */
#define PT_TRACED 0x01 /* this process is being traced */
#define PT_TRACE_SYSCALL 0x02 /* stop at the next syscall entry */
#define PT_STOPPED 0x04 /* stopped, waiting for tracer action */
#define PT_STOP_REPORTED 0x08 /* stop already returned to tracer via waitpid */
#define PT_SINGLESTEP 0x10 /* TF set; stop after next instruction */

/* SIGTRAP delivered to tracer on ptrace-stop (matches Linux) */
#define SIGTRAP 5

/*
 * PEEKUSER / POKEUSER byte offsets into user_regs_struct (i386 layout).
 * These match the Linux kernel's user_regs_struct for 32-bit i386.
 */
#define PT_OFF_EBX 0
#define PT_OFF_ECX 4
#define PT_OFF_EDX 8
#define PT_OFF_ESI 12
#define PT_OFF_EDI 16
#define PT_OFF_EBP 20
#define PT_OFF_EAX 24
#define PT_OFF_DS 28
#define PT_OFF_ES 32
#define PT_OFF_FS 36
#define PT_OFF_GS 40
#define PT_OFF_ORIG_EAX 44
#define PT_OFF_EIP 48
#define PT_OFF_CS 52
#define PT_OFF_EFLAGS 56
#define PT_OFF_ESP 60
#define PT_OFF_SS 64
#define PT_OFF_MAX 68 /* first invalid offset */

/* EFLAGS Trap Flag — enables single-step mode on the CPU */
#define EFLAGS_TF 0x00000100

/*
 * user_regs_struct — register snapshot for PTRACE_GETREGS / PTRACE_SETREGS.
 * Field order and names match the Linux i386 struct so that tools compiled
 * against Linux headers work without modification.
 */
struct user_regs_struct {
	unsigned long ebx;
	unsigned long ecx;
	unsigned long edx;
	unsigned long esi;
	unsigned long edi;
	unsigned long ebp;
	unsigned long eax;
	unsigned long xds;
	unsigned long xes;
	unsigned long xfs;
	unsigned long xgs;
	unsigned long orig_eax;
	unsigned long eip;
	unsigned long xcs;
	unsigned long eflags;
	unsigned long esp;
	unsigned long xss;
};

/*
 * TASK_INTR_FRAME(task) — return a pointer to the saved intr_frame that sits
 * at the very top of a task's kernel-stack page.  do_fork() uses the same
 * formula, so this is canonical for this kernel.
 */
#define TASK_INTR_FRAME(task) \
	((intr_frame *)((char *)(task) + PAGE_SIZE - sizeof(intr_frame)))

int sys_ptrace(unsigned long request, unsigned long pid, unsigned long addr,
	       unsigned long data);

void ptrace_stop(intr_frame *frame);

#endif /* _PTRACE_H_ */
