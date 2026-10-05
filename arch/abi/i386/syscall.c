/*
 * syscall.c — syscall dispatch table and interrupt handler.
 *
 * Individual handlers are implemented in:
 *   syscall_io.c   — file descriptor I/O
 *   syscall_fs.c   — filesystem / path operations
 *   syscall_proc.c — process management
 *   syscall_sys.c  — system / miscellaneous
 *
 * Handlers defined outside the syscall layer:
 *   ps/ps_syscall.c — sys_exit, sys_fork, sys_vfork, sys_waitpid, sys_getcwd,
 *                     sys_getrusage
 *   ps/ps_signal.c  — signal delivery and signal-related syscalls
 *   elf/exec.c      — sys_execve
 *   fs/syslog.c     — sys_syslog
 */

#include <int/int.h>
#include <ps/ps.h>
#include <elf/exec.h>
#include <lib/klib.h>
#include <ps/signal.h>
#include <device/time.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <arch/abi/i386/compat.h>
#include <syscall/syscall.h>
#include "services.h"

/* handlers defined in other subsystems */
int sys_getrusage(int who, rusage *usage);
int sys_syslog(int type, char *buf, int len);
typedef intptr_t (*syscall_fn)(intr_frame *);

int test_call(unsigned arg0, unsigned arg1, unsigned arg2)
{
	printk("test call: arg0 %x, arg1 %x, arg2 %x\n", arg0, arg1, arg2);
	return 0;
}

#define ARG0 ((uint32_t)frame->ebx)
#define ARG1 ((uint32_t)frame->ecx)
#define ARG2 ((uint32_t)frame->edx)
#define ARG3 ((uint32_t)frame->esi)
#define ARG4 ((uint32_t)frame->edi)
#define ARG5 ((uint32_t)frame->ebp)
#define PTR0 ((void *)(uintptr_t)ARG0)
#define PTR1 ((void *)(uintptr_t)ARG1)
#define PTR2 ((void *)(uintptr_t)ARG2)
#define PTR3 ((void *)(uintptr_t)ARG3)
#define PTR4 ((void *)(uintptr_t)ARG4)
#define PTR5 ((void *)(uintptr_t)ARG5)

#define I386_SYSCALL(number, name, service)                   \
	static intptr_t entry_##name(intr_frame *frame        \
				     __attribute__((unused))) \
	{                                                     \
		return service;                               \
	}
#include "calls.def"
#undef I386_SYSCALL

static const syscall_fn call_table[NR_syscalls] = {
#define I386_SYSCALL(number, name, service) [number] = entry_##name,
#include "calls.def"
#undef I386_SYSCALL
};

static int unhandled_syscall(unsigned callno)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("unhandled syscall %d\n", callno);
	return -ENOSYS;
}

void i386_syscall_process(intr_frame *frame)
{
	syscall_fn fn;
	int ret;
	int traced;

	/* A tracer may enable syscall stops while this call is already running. */
	traced = ps_ptrace_maybe_stop_syscall(frame, 1);

	if (frame->eax >= NR_syscalls) {
		frame->eax = unhandled_syscall(frame->eax);
		if (traced)
			ps_ptrace_maybe_stop_syscall(frame, 0);
		return;
	}

	fn = call_table[frame->eax];
	if (!fn)
		ret = unhandled_syscall(frame->eax);
	else
		ret = fn(frame);

	frame->eax = ret;
	if (traced)
		ps_ptrace_maybe_stop_syscall(frame, 0);
}
