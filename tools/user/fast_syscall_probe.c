/* Freestanding entry/return regression, built by test/fast_syscall.py. */
typedef unsigned long word;
typedef long result;
#ifdef __x86_64__
#define NR_WRITE 1
#define NR_OPEN 2
#define NR_CLOSE 3
#define NR_MMAP 9
#define NR_MUNMAP 11
#define NR_SIGACTION 13
#define NR_GETPID 39
#define NR_FORK 57
#define NR_EXIT 60
#define NR_WAIT 61
#define NR_KILL 62
#define NR_YIELD 24
#define NR_IOPL 172
#define NR_PTRACE 101
static result call6(word n, word a, word b, word c, word d, word e, word f)
{
	register word r10 asm("r10") = d, r8 asm("r8") = e, r9 asm("r9") = f;
	result ret;
	asm volatile("syscall"
		     : "=a"(ret)
		     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
		       "r"(r9)
		     : "rcx", "r11", "memory", "cc");
	return ret;
}
__attribute__((naked)) static word flag_probe(void)
{
	asm volatile("std; mov $39,%eax; syscall; pushfq; pop %rax; cld; ret");
}
__attribute__((naked)) static void restorer(void)
{
	asm volatile("mov $15,%eax; syscall; ud2");
}
struct action {
	word handler, flags, restorer, mask;
};
static word observed_rcx, observed_r11;
#else
#define NR_WRITE 4
#define NR_OPEN 5
#define NR_CLOSE 6
#define NR_MMAP 192
#define NR_MUNMAP 91
#define NR_SIGACTION 174
#define NR_GETPID 20
#define NR_FORK 2
#define NR_EXIT 1
#define NR_WAIT 114
#define NR_KILL 37
#define NR_YIELD 158
#define NR_IOPL 110
#define NR_PTRACE 26
/* AT_SYSINFO is chosen by the kernel, including CPUs without SEP. */
word fast_entry;
__attribute__((naked, noinline, noclone)) static result
call6(word n, word a, word b, word c, word d, word e, word f)
{
	asm volatile("push %ebp; push %edi; push %esi; push %ebx;"
		     "mov 20(%esp),%eax; mov 24(%esp),%ebx;"
		     "mov 28(%esp),%ecx; mov 32(%esp),%edx;"
		     "mov 36(%esp),%esi; mov 40(%esp),%edi;"
		     "mov 44(%esp),%ebp; cmpl $0,fast_entry; je 1f;"
		     "call *fast_entry; jmp 2f; 1: int $0x80;"
		     "2: pop %ebx; pop %esi; pop %edi; pop %ebp; ret");
}
__attribute__((naked)) static word flag_probe(void)
{
	asm volatile("std; mov $20,%eax; cmpl $0,fast_entry; je 1f;"
		     "call *fast_entry; jmp 2f; 1: int $0x80;"
		     "2: pushfl; pop %eax; cld; ret");
}
__attribute__((naked)) static void restorer(void)
{
	/* rt_sigreturn must enter with the signal-frame stack untouched. */
	asm volatile("mov $173,%eax; int $0x80; ud2");
}
struct action {
	word handler, flags, restorer, mask[2];
};
#endif
static void print(const char *s)
{
	/* The probe has IOPL=3; write directly to QEMU's COM1. */
	while (*s) {
		unsigned char ready;
		do {
			asm volatile("inb %w1,%0" : "=a"(ready) : "d"(0x3fd));
		} while (!(ready & 0x20));
		asm volatile("outb %b0,%w1" : : "a"(*s++), "d"(0x3f8));
	}
}
__attribute__((noreturn)) static void finish(int code)
{
	if (!call6(NR_IOPL, 3, 0, 0, 0, 0, 0))
		asm volatile("outb %b0,$0xf4" : : "a"(code));
	call6(NR_EXIT, code, 0, 0, 0, 0, 0);
	for (;;)
		asm volatile("ud2");
}
static void check(int ok, const char *name)
{
	if (!ok) {
		print("FAIL ");
		print(name);
		print("\n");
		finish(1);
	}
}
static volatile int delivered;
static void handler(int sig, void *info, void *context)
{
	(void)info;
	if (sig == 10)
		delivered++;
#ifdef __x86_64__
	/* AMD64 ucontext has mcontext at byte 40; RCX/R11 are slots 14/3.
	 * These edits require IRET, which must preserve both registers. */
	word *regs = (word *)((char *)context + 40);
	regs[14] = 0x12345678;
	regs[3] = 0x87654321;
#else
	(void)context;
#endif
}
__attribute__((used)) static void probe_main(word *stack)
{
#ifndef __x86_64__
	word *aux = stack + 1 + stack[0] + 1;
	while (*aux++) {
	}
	for (; aux[0]; aux += 2)
		if (aux[0] == 32)
			fast_entry = aux[1];
#endif
	if (call6(NR_IOPL, 3, 0, 0, 0, 0, 0))
		finish(2);
#ifndef __x86_64__
	check(fast_entry != 0, "AT_SYSINFO");
	unsigned char *entry = (void *)fast_entry;
	if (entry[0] == 0xf3 && entry[1] == 0x0f && entry[2] == 0x1e)
		entry += 4; /* compiler-emitted ENDBR landing instruction */
	int uses_sysenter = entry[0] == 0x51 && entry[5] == 0x0f &&
			    entry[6] == 0x34;
	if (uses_sysenter)
		print("ENTRY SYSENTER\n");
	else {
		check(entry[0] == 0xcd && entry[1] == 0x80, "vDSO bytes");
		print("ENTRY INT80\n");
	}
#else
	(void)stack;
	print("ENTRY SYSCALL\n");
#endif
	word pid = call6(NR_GETPID, 0, 0, 0, 0, 0, 0);
	check(pid == 1, "getpid");
	check(call6(~0UL, 0, 0, 0, 0, 0, 0) == -38, "ENOSYS");
	check(flag_probe() & 0x400, "DF restored");
	/* All six arguments: map the second page of a real file. */
	result fd = call6(NR_OPEN, (word) "/root/fastcall", 0, 0, 0, 0, 0);
	check(fd >= 0, "open file");
#ifdef __x86_64__
	word offset = 4096;
#else
	word offset = 1;
#endif
	result map = call6(NR_MMAP, 0, 4096, 1, 2, fd, offset);
	check((word)map < (word)-4095, "mmap six args");
	check(*(volatile unsigned char *)map == 0x5a, "mmap sixth arg");
	check(!call6(NR_MUNMAP, map, 4096, 0, 0, 0, 0), "munmap");
	call6(NR_CLOSE, fd, 0, 0, 0, 0, 0);
	struct action action = { .handler = (word)handler,
				 .flags = 0x04000004,
				 .restorer = (word)restorer };
	check(!call6(NR_SIGACTION, 10, (word)&action, 0, 8, 0, 0), "sigaction");
#ifdef __x86_64__
	result ret;
	asm volatile("syscall; mov %%rcx,%1; mov %%r11,%2"
		     : "=a"(ret), "=m"(observed_rcx), "=m"(observed_r11)
		     : "a"((word)NR_KILL), "D"(pid), "S"((word)10)
		     : "rcx", "r11", "memory", "cc");
	check(!ret && delivered == 1, "signal return");
	check(observed_rcx == 0x12345678 && observed_r11 == 0x87654321,
	      "edited RCX/R11 IRET fallback");
#else
	check(!call6(NR_KILL, pid, 10, 0, 0, 0, 0) && delivered == 1,
	      "signal return");
#endif
	for (unsigned i = 0; i < 4; i++) {
		result child = call6(NR_FORK, 0, 0, 0, 0, 0, 0);
		check(child >= 0, "fork");
		if (!child) {
			for (unsigned j = 0; j < 64; j++) {
				call6(NR_YIELD, 0, 0, 0, 0, 0, 0);
				if (call6(NR_GETPID, 0, 0, 0, 0, 0, 0) > 1 &&
				    (flag_probe() & 0x400))
					continue;
				call6(NR_EXIT, 1, 0, 0, 0, 0, 0);
			}
			call6(NR_EXIT, 0, 0, 0, 0, 0, 0);
		}
		int status = -1;
		check(call6(NR_WAIT, child, (word)&status, 0, 0, 0, 0) ==
				      child &&
			      !status,
		      "fork/wait/SMP");
	}
	/* Syscall tracing must see the same entry/exit stops on both ABIs. */
	result traced = call6(NR_FORK, 0, 0, 0, 0, 0, 0);
	check(traced >= 0, "ptrace fork");
	if (!traced) {
		check(!call6(NR_PTRACE, 0, 0, 0, 0, 0, 0), "TRACEME");
		word self = call6(NR_GETPID, 0, 0, 0, 0, 0, 0);
		call6(NR_KILL, self, 19, 0, 0, 0, 0);
		call6(NR_GETPID, 0, 0, 0, 0, 0, 0);
		call6(NR_EXIT, 0, 0, 0, 0, 0, 0);
		for (;;) {
		}
	}
	int trace_status = -1;
	check(call6(NR_WAIT, traced, (word)&trace_status, 0, 0, 0, 0) ==
			      traced &&
		      (trace_status & 255) == 127,
	      "ptrace initial stop");
	unsigned stops = 0;
	for (;;) {
		check(!call6(NR_PTRACE, 24, traced, 0, 0, 0, 0),
		      "PTRACE_SYSCALL");
		check(call6(NR_WAIT, traced, (word)&trace_status, 0, 0, 0, 0) ==
			      traced,
		      "ptrace wait");
		if ((trace_status & 255) != 127)
			break;
		check((trace_status >> 8) == 5 && ++stops <= 8,
		      "syscall trace stop");
	}
	check(!trace_status && stops >= 3, "syscall entry/exit tracing");
#ifndef __x86_64__
	if (uses_sysenter) {
		result child = call6(NR_FORK, 0, 0, 0, 0, 0, 0);
		check(child >= 0, "bad stack fork");
		if (!child) {
			asm volatile(
				"mov $0xc0000000,%%ebp; mov $20,%%eax; sysenter; ud2"
				:
				:
				: "memory");
			for (;;) {
			}
		}
		int status = -1;
		check(call6(NR_WAIT, child, (word)&status, 0, 0, 0, 0) ==
				      child &&
			      (status & 127) == 11,
		      "bad SYSENTER stack gives SIGSEGV");
	}
	word entry_saved = fast_entry;
	fast_entry = 0;
	check(call6(NR_GETPID, 0, 0, 0, 0, 0, 0) == pid, "legacy INT80");
	fast_entry = entry_saved;
#endif
	print("FAST_SYSCALL_PASS\n");
	finish(0);
}
__attribute__((naked, noreturn)) void _start(void)
{
#ifdef __x86_64__
	asm volatile("mov %rsp,%rdi; and $-16,%rsp; call probe_main; ud2");
#else
	asm volatile(
		"mov %esp,%eax; and $-16,%esp; sub $12,%esp; push %eax; call probe_main; ud2");
#endif
}
