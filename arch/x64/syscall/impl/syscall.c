/* AMD64 and IA-32 syscall namespaces remain separate. */
#include <ps/ps.h>
#include <ps/task.h>
#include <int/int.h>
#include <mm/mm.h>
#include <elf/exec.h>
#include <net/sock.h>
#include <macro.h>
#include <errno.h>
#include <syscall/syscall.h>
#include "native.h"

extern void i386_syscall_process(intr_frame *);
extern int sys_getrusage(int, rusage *);
extern int sys_syslog(int, char *, int);
extern int test_call(unsigned, unsigned, unsigned);

typedef intptr_t (*native_fn)(intr_frame *);

#define ARG0 (frame->edi)
#define ARG1 (frame->esi)
#define ARG2 (frame->edx)
#define ARG3 (frame->r10)
#define ARG4 (frame->r8)
#define ARG5 (frame->r9)
#define PTR0 ((void *)ARG0)
#define PTR1 ((void *)ARG1)
#define PTR2 ((void *)ARG2)
#define PTR3 ((void *)ARG3)
#define PTR4 ((void *)ARG4)
#define PTR5 ((void *)ARG5)

#define NATIVE_SYSCALL(number, name, service)                 \
	static intptr_t entry_##name(intr_frame *frame        \
				     __attribute__((unused))) \
	{                                                     \
		return service;                               \
	}
#include "calls.def"
#undef NATIVE_SYSCALL

static const native_fn native_calls[442] = {
#define NATIVE_SYSCALL(number, name, service) [number] = entry_##name,
#include "calls.def"
#undef NATIVE_SYSCALL
};

static intptr_t native_process(intr_frame *frame)
{
	unsigned long number = frame->eax;
	if (number >= sizeof(native_calls) / sizeof(native_calls[0]) ||
	    !native_calls[number])
		return -ENOSYS;
	return native_calls[number](frame);
}

static void syscall_process(intr_frame *frame)
{
	if (frame->cs != USER64_CODE_SELECTOR) {
		i386_syscall_process(frame);
		return;
	}
	int traced = ps_ptrace_maybe_stop_syscall(frame, 1);
	frame->eax = native_process(frame);
	if (traced)
		ps_ptrace_maybe_stop_syscall(frame, 0);
}
static void syscall_init(void)
{
	int_register(SYSCALL_INT_NO, syscall_process, 0, 3);
}
KERNEL_INIT(7, syscall_init);
