/* Freestanding shebang argument/error regression and exec benchmark. */
typedef unsigned long word;
typedef long result;
#ifdef __x86_64__
#define NR_WRITE 1
#define NR_MOUNT 165
#define NR_OPEN 2
#define NR_CLOSE 3
#define NR_FORK 57
#define NR_EXEC 59
#define NR_EXIT 60
#define NR_WAIT 61
#define NR_FCNTL 72
#define NR_TIME 96
#define NR_IOPL 172
static result call(word n, word a, word b, word c, word d, word e)
{
	register word r10 asm("r10") = d, r8 asm("r8") = e;
	result ret;
	asm volatile("syscall" : "=a"(ret)
		     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
		     : "rcx", "r11", "memory", "cc");
	return ret;
}
#else
#define NR_WRITE 4
#define NR_MOUNT 21
#define NR_OPEN 5
#define NR_CLOSE 6
#define NR_FORK 2
#define NR_EXEC 11
#define NR_EXIT 1
#define NR_WAIT 114
#define NR_FCNTL 55
#define NR_TIME 78
#define NR_IOPL 110
word fast_entry;
__attribute__((naked, noinline, noclone)) static result
call(word n, word a, word b, word c, word d, word e)
{
	asm volatile("push %ebp; push %edi; push %esi; push %ebx;"
		     "mov 20(%esp),%eax; mov 24(%esp),%ebx;"
		     "mov 28(%esp),%ecx; mov 32(%esp),%edx;"
		     "mov 36(%esp),%esi; mov 40(%esp),%edi;"
		     "cmpl $0,fast_entry; je 1f; call *fast_entry; jmp 2f;"
		     "1: int $0x80; 2: pop %ebx; pop %esi; pop %edi; pop %ebp; ret");
}
#endif
#ifndef EXEC_ITERATIONS
#define EXEC_ITERATIONS 100
#endif
#ifndef FAILED_EXEC_ITERATIONS
#define FAILED_EXEC_ITERATIONS 10000
#endif

static void print(const char *s)
{
	while (*s) {
		unsigned char ready;
		do {
			asm volatile("inb %w1,%0" : "=a"(ready) : "d"(0x3fd));
		} while (!(ready & 0x20));
		asm volatile("outb %b0,%w1" : : "a"(*s++), "d"(0x3f8));
	}
}
static void decimal(word n)
{
	char digits[32]; unsigned i = 0;
	do { digits[i++] = '0' + n % 10; n /= 10; } while (n);
	while (i) { char digit[2] = { digits[--i], 0 }; print(digit); }
}
__attribute__((noreturn)) static void finish(int code)
{
	asm volatile("outb %b0,$0xf4" : : "a"(code));
	call(NR_EXIT, code, 0, 0, 0, 0);
	for (;;) asm volatile("ud2");
}
static void check(int ok, const char *name)
{
	if (!ok) { print("FAIL "); print(name); print("\n"); finish(1); }
}
static int equal(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}
static char fd_env[32];
static char *environment[] = { "SHEBANG_TEST=ok", "EMPTY=", fd_env, 0 };
static char *arguments[] = { "custom argv0", "tail", 0 };
static result held_fd;

static void set_fd_env(unsigned fd)
{
	const char *prefix = "FD_TEST=";
	unsigned n = 0, i = 0;
	char digits[16];
	while (prefix[n]) { fd_env[n] = prefix[n]; n++; }
	do { digits[i++] = '0' + fd % 10; fd /= 10; } while (fd);
	while (i) fd_env[n++] = digits[--i];
	fd_env[n] = 0;
}
static void validate_child(word argc, char **argv, char **envp)
{
	check(envp && envp[0] && envp[1] && envp[2] && !envp[3], "environment count");
	check(equal(envp[0], "SHEBANG_TEST=ok") && equal(envp[1], "EMPTY="), "environment values");
	const char *fd_text = envp[2];
	const char *prefix = "FD_TEST=";
	unsigned i, fd = 0;
	for (i = 0; prefix[i]; i++) check(fd_text[i] == prefix[i], "descriptor environment");
	for (; fd_text[i]; i++) fd = fd * 10 + fd_text[i] - '0';
	check(call(NR_FCNTL, fd, 1, 0, 0, 0) == -9, "CLOEXEC after successful exec");
	check(call(NR_FCNTL, 65, 1, 0, 0, 0) == -9, "second CLOEXEC bitmap word");
	check(call(NR_FCNTL, 1001, 1, 0, 0, 0) == 0, "ordinary descriptor survives exec");
	if (argc == 3 && (equal(argv[1], "--elf-chain") ||
			  equal(argv[1], "/root/shebang-chain"))) {
		unsigned remaining = 0;
		for (const char *p = argv[2]; *p; p++) remaining = remaining * 10 + *p - '0';
		if (remaining > 1) {
			char next[16], reverse[16];
			unsigned i = 0, n = 0;
			remaining--;
			do { reverse[i++] = '0' + remaining % 10; remaining /= 10; } while (remaining);
			while (i) next[n++] = reverse[--i];
			next[n] = 0;
			int elf = equal(argv[1], "--elf-chain");
			char *elf_args[] = { "chain", "--elf-chain", next, 0 };
			char *script_args[] = { "chain", next, 0 };
			call(NR_EXEC, (word)(elf ? "/bin/shebang-probe" : "/root/shebang-chain"),
			     (word)(elf ? elf_args : script_args), (word)envp, 0, 0);
			check(0, "repeated exec chain");
		}
	} else if (argc >= 2 && equal(argv[1], "--elf-child")) {
		check(argc == 3 && equal(argv[0], "custom ELF argv0") && equal(argv[2], "tail"), "ELF argv unchanged");
	} else if (argc >= 2 && equal(argv[1], "-u -O  ")) {
		check(argc == 5 && equal(argv[0], "/bin/shebang-probe") &&
		      equal(argv[2], "/root/shebang-arg") && equal(argv[3], "") &&
		      equal(argv[4], "tail value"), "optional interpreter argument and empty argv");
	} else if (argc >= 2 && equal(argv[1], "/root/shebang-empty-argv")) {
		check(argc == 2 && equal(argv[0], "/bin/shebang-probe"), "empty original argv");
	} else {
		check(argc == 3 && equal(argv[0], "/bin/shebang-probe") &&
		      (equal(argv[1], "/root/shebang-basic") || equal(argv[1], "/root/shebang-no-newline")) &&
		      equal(argv[2], "tail"), "script argv order");
	}
	call(NR_EXIT, 0, 0, 0, 0, 0);
	for (;;) {}
}
static void run_exec(const char *path, char **argv)
{
	result child = call(NR_FORK, 0, 0, 0, 0, 0);
	check(child >= 0, "fork exec child");
	if (!child) {
		result ret = call(NR_EXEC, (word)path, (word)argv, (word)environment, 0, 0);
		print("FAIL exec "); print(path); print(" errno="); decimal(-ret); print("\n");
		finish(1);
	}
	int status = -1;
	check(call(NR_WAIT, child, (word)&status, 0, 0, 0) == child && status == 0, "exec child status");
}
static void failed_exec(const char *path, int expected)
{
	result ret = call(NR_EXEC, (word)path, (word)arguments, (word)environment, 0, 0);
	check(ret == -expected, path);
	check(call(NR_FCNTL, held_fd, 1, 0, 0, 0) == 1, "failed exec retains CLOEXEC descriptor");
}
static void benchmark(const char *kind, const char *path, char **argv,
		      unsigned count, int failed)
{
	struct { long sec, usec; } start, end;
	check(!call(NR_TIME, (word)&start, 0, 0, 0, 0), "start clock");
	unsigned i;
	for (i = 0; i < count; i++) {
		if (failed) failed_exec(path, 2);
		else run_exec(path, argv);
	}
	check(!call(NR_TIME, (word)&end, 0, 0, 0, 0), "end clock");
	word elapsed = (end.sec - start.sec) * 1000000 + end.usec - start.usec;
	check(elapsed > 0, "positive benchmark duration");
	print("BENCH "); print(kind); print(" count="); decimal(count);
	print(" elapsed_us="); decimal(elapsed); print("\n");
}
__attribute__((used)) static void probe_main(word *stack)
{
	word argc = stack[0];
	char **argv = (void *)(stack + 1);
	char **envp = argv + argc + 1;
#ifndef __x86_64__
	word *aux = (void *)envp;
	while (*aux) aux++;
	for (aux++; aux[0]; aux += 2) if (aux[0] == 32) fast_entry = aux[1];
#endif
	check(!call(NR_IOPL, 3, 0, 0, 0, 0), "IOPL");
	if (argc != 1 || !equal(argv[0], "/sbin/init")) validate_child(argc, argv, envp);
#ifdef EXEC_KERNEL_TESTS
	check(!call(NR_MOUNT, (word)"proc", (word)"/proc", (word)"proc", 0, 0), "mount proc");
	result runner = call(NR_OPEN, (word)"/proc/tests/.runner", 1, 0, 0, 0);
	check(runner >= 0, "open ELF test runner");
	check(call(NR_WRITE, runner, (word)"ElfTest", 7, 0, 0) == 7, "run ELF kernel tests");
	check(!call(NR_CLOSE, runner, 0, 0, 0, 0), "close ELF test runner");
#endif
	held_fd = call(NR_OPEN, (word)"/bin/shebang-probe", 02000000, 0, 0, 0);
	check(held_fd >= 0, "open CLOEXEC test file");
	/* Exercise a bitmap word far beyond the usual low descriptors. */
	result high_fd = call(NR_FCNTL, held_fd, 1030, 1000, 0, 0);
	check(high_fd == 1000, "duplicate high CLOEXEC descriptor");
	check(call(NR_FCNTL, held_fd, 1030, 65, 0, 0) == 65, "duplicate second CLOEXEC descriptor");
	check(call(NR_FCNTL, held_fd, 0, 1001, 0, 0) == 1001, "duplicate ordinary descriptor");
	check(!call(NR_CLOSE, held_fd, 0, 0, 0, 0), "close low descriptor");
	held_fd = high_fd;
	set_fd_env(held_fd);
	char *elf_argv[] = { "custom ELF argv0", "--elf-child", "tail", 0 };
	char *arg_argv[] = { "custom ignored", "", "tail value", 0 };
	run_exec("/root/shebang-basic", arguments);
	run_exec("/root/shebang-arg", arg_argv);
	run_exec("/bin/shebang-probe", elf_argv);
	char count[16], reverse[16];
	unsigned remaining = EXEC_ITERATIONS, n = 0, j = 0;
	do { reverse[j++] = '0' + remaining % 10; remaining /= 10; } while (remaining);
	while (j) count[n++] = reverse[--j];
	count[n] = 0;
	char *elf_chain[] = { "chain", "--elf-chain", count, 0 };
	char *script_chain[] = { "chain", count, 0 };
	unsigned repeat;
	for (repeat = 0; repeat < 3; repeat++) {
		benchmark("elf", "/bin/shebang-probe", elf_argv, EXEC_ITERATIONS, 0);
		benchmark("script", "/root/shebang-basic", arguments, EXEC_ITERATIONS, 0);
		benchmark("failed-script", "/root/shebang-missing", arguments, FAILED_EXEC_ITERATIONS, 1);
		struct { long sec, usec; } start, end;
		check(!call(NR_TIME, (word)&start, 0, 0, 0, 0), "chain start clock");
		run_exec("/bin/shebang-probe", elf_chain);
		check(!call(NR_TIME, (word)&end, 0, 0, 0, 0), "chain end clock");
		print("BENCH elf-chain count="); decimal(EXEC_ITERATIONS);
		print(" elapsed_us="); decimal((end.sec-start.sec)*1000000 + end.usec-start.usec); print("\n");
		check(!call(NR_TIME, (word)&start, 0, 0, 0, 0), "chain start clock");
		run_exec("/root/shebang-chain", script_chain);
		check(!call(NR_TIME, (word)&end, 0, 0, 0, 0), "chain end clock");
		print("BENCH script-chain count="); decimal(EXEC_ITERATIONS);
		print(" elapsed_us="); decimal((end.sec-start.sec)*1000000 + end.usec-start.usec); print("\n");
	}
#ifndef SHEBANG_BENCH_ONLY
	run_exec("/root/shebang-no-newline", arguments);
	run_exec("/root/shebang-empty-argv", 0);
	failed_exec("/root/shebang-empty", 8);
	failed_exec("/root/shebang-truncated", 8);
	failed_exec("/root/shebang-missing", 2);
	failed_exec("/root/shebang-invalid-interp", 8);
	failed_exec("/root/shebang-no-exec", 1);
	failed_exec("/root/elf-short", 8);
	failed_exec("/root/elf-machine", 8);
	failed_exec("/root/elf-data", 8);
	failed_exec("/root/elf-phdr", 8);
	print("SHEBANG_PROBE_PASS\n");
#else
	print("SHEBANG_BENCHMARK_PASS\n");
#endif
	check(!call(NR_CLOSE, held_fd, 0, 0, 0, 0), "close held descriptor");
	check(!call(NR_CLOSE, 65, 0, 0, 0, 0), "close second CLOEXEC descriptor");
	check(!call(NR_CLOSE, 1001, 0, 0, 0, 0), "close ordinary descriptor");
	finish(0);
}
__attribute__((naked, noreturn)) void _start(void)
{
#ifdef __x86_64__
	asm volatile("mov %rsp,%rdi; and $-16,%rsp; call probe_main; ud2");
#else
	asm volatile("mov %esp,%eax; and $-16,%esp; sub $12,%esp; push %eax; call probe_main; ud2");
#endif
}
