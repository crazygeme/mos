/* Freestanding filesystem regression and open/close benchmark for QEMU. */
typedef unsigned long word;
typedef long result;
#ifdef __x86_64__
#define NR_READ 0
#define NR_WRITE 1
#define NR_OPEN 2
#define NR_CLOSE 3
#define NR_DUP 32
#define NR_SEEK 8
#define NR_INOTIFY_INIT 294
#define NR_INOTIFY_ADD 254
#define NR_FORK 57
#define NR_EXIT 60
#define NR_WAIT 61
#define NR_MKDIR 83
#define NR_CHDIR 80
#define NR_UNLINK 87
#define NR_SYMLINK 88
#define NR_READLINK 89
#define NR_RENAME 82
#define NR_LINK 86
#define NR_CHMOD 90
#define NR_GETTIME 96
#define NR_SETUID 105
#define NR_IOPL 172
#define NR_MOUNT 165
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
#define NR_READ 3
#define NR_WRITE 4
#define NR_OPEN 5
#define NR_CLOSE 6
#define NR_DUP 41
#define NR_SEEK 19
#define NR_INOTIFY_INIT 332
#define NR_INOTIFY_ADD 292
#define NR_FORK 2
#define NR_EXIT 1
#define NR_WAIT 114
#define NR_MKDIR 39
#define NR_CHDIR 12
#define NR_UNLINK 10
#define NR_SYMLINK 83
#define NR_READLINK 85
#define NR_RENAME 38
#define NR_LINK 9
#define NR_CHMOD 15
#define NR_GETTIME 78
#define NR_SETUID 213
#define NR_IOPL 110
#define NR_MOUNT 21
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
#define O_CREAT 64
#define O_EXCL 128
#define O_TRUNC 512
#define O_DIRECTORY 65536
#define O_NOFOLLOW 131072
#define O_PATH 010000000
#ifndef OPEN_CLOSE_PAIRS
#define OPEN_CLOSE_PAIRS 20000
#endif
#ifndef OPEN_CLOSE_REPEATS
#define OPEN_CLOSE_REPEATS 1
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
	char digits[32];
	unsigned i = 0;
	do {
		digits[i++] = '0' + n % 10;
		n /= 10;
	} while (n);
	while (i) {
		char digit[2] = { digits[--i], 0 };
		print(digit);
	}
}
__attribute__((noreturn)) static void finish(int code)
{
	asm volatile("outb %b0,$0xf4" : : "a"(code));
	call(NR_EXIT, code, 0, 0, 0, 0);
	for (;;)
		asm volatile("ud2");
}
static void check(int ok, const char *name)
{
	if (!ok) {
		print("FAIL "); print(name); print("\n"); finish(1);
	}
}
static result open_file(const char *path, int flags)
{
	return call(NR_OPEN, (word)path, flags, 0600, 0, 0);
}
static void close_file(result fd)
{
	check(fd >= 0 && !call(NR_CLOSE, fd, 0, 0, 0, 0), "close");
}
static void write_byte(result fd, char value)
{
	check(call(NR_WRITE, fd, (word)&value, 1, 0, 0) == 1, "write byte");
}
static void contents(const char *path, char expected)
{
	char value = 0;
	result fd = open_file(path, 0);
	check(fd >= 0, path);
	check(call(NR_READ, fd, (word)&value, 1, 0, 0) == 1 && value == expected, path);
	close_file(fd);
}
static void create(const char *path, char value)
{
	result fd = open_file(path, 2 | O_CREAT | O_EXCL);
	check(fd >= 0, "create regular file");
	write_byte(fd, value);
	close_file(fd);
}
static void unlinked_duplicate(const char *path, char expected)
{
	result fd = open_file(path, 0);
	check(fd >= 0, "open before duplicate");
	result alias = call(NR_DUP, fd, 0, 0, 0, 0);
	check(alias >= 0, "duplicate descriptor");
	check(!call(NR_UNLINK, (word)path, 0, 0, 0, 0), "unlink duplicated file");
	close_file(fd);
	char value = 0;
	check(call(NR_READ, alias, (word)&value, 1, 0, 0) == 1 && value == expected,
	      "duplicated descriptor retains unlinked file");
	check(call(NR_SEEK, alias, 0, 0, 0, 0) == 0, "seek retained file");
	value = 0;
	check(call(NR_READ, alias, (word)&value, 1, 0, 0) == 1 && value == expected,
	      "retained inode remains valid");
	close_file(alias);
}
static void watch_open_close(const char *path)
{
	result notify = call(NR_INOTIFY_INIT, 2048, 0, 0, 0, 0);
	check(notify >= 0, "inotify init");
	result wd = call(NR_INOTIFY_ADD, notify, (word)path, 0x30, 0, 0);
	check(wd >= 0, "watch benchmark file");
	close_file(open_file(path, 0));
	char buffer[256];
	result length = call(NR_READ, notify, (word)buffer, sizeof(buffer), 0, 0);
	check(length > 0, "read open/close events");
	unsigned offset = 0, mask = 0;
	while (offset < (word)length) {
		struct event { int wd; unsigned mask, cookie, length; };
		struct event *event = (void *)(buffer + offset);
		check(offset + sizeof(*event) <= (word)length && event->wd == wd,
		      "valid inotify event");
		mask |= event->mask;
		offset += sizeof(*event) + event->length;
	}
	check(offset == (word)length && (mask & 0x30) == 0x30,
	      "inotify reports both open and close");
	close_file(notify);
}
static void benchmark(const char *kind, const char *path)
{
	struct { long sec, usec; } start, end;
	unsigned i;
#ifdef OPEN_CLOSE_PROFILE
	print("PROFILE_BEGIN "); print(kind); print("\n");
#endif
	for (i = 0; i < 1000; ++i)
		close_file(open_file(path, 0));
	check(!call(NR_GETTIME, (word)&start, 0, 0, 0, 0), "start clock");
	for (i = 0; i < OPEN_CLOSE_PAIRS; ++i)
		close_file(open_file(path, 0));
	check(!call(NR_GETTIME, (word)&end, 0, 0, 0, 0), "end clock");
	long elapsed = (end.sec - start.sec) * 1000000 + end.usec - start.usec;
	check(elapsed > 0, "positive benchmark time");
	print("BENCH "); print(kind); print(" pairs="); decimal(OPEN_CLOSE_PAIRS);
	print(" elapsed_us=");
	decimal(elapsed); print("\n");
#ifdef OPEN_CLOSE_PROFILE
	print("PROFILE_END "); print(kind); print("\n");
#endif
}
__attribute__((used)) static void probe_main(word *stack)
{
#ifndef __x86_64__
	word *aux = stack + 1 + stack[0] + 1;
	while (*aux) aux++;
	for (aux++; aux[0]; aux += 2)
		if (aux[0] == 32) fast_entry = aux[1];
#else
	(void)stack;
#endif
	check(!call(NR_IOPL, 3, 0, 0, 0, 0), "IOPL");
	check(!call(NR_MOUNT, 0, (word)"/", (word)"ext4", 32, 0), "remount rw");
	check(!call(NR_MKDIR, (word)"/tmp/open-close", 0700, 0, 0, 0), "mkdir");
	create("/tmp/open-close/file", 'a');
	check(!call(NR_SYMLINK, (word)"file", (word)"/tmp/open-close/link", 0, 0, 0), "relative symlink");
	check(!call(NR_MKDIR, (word)"/tmp/open-close/mount", 0700, 0, 0, 0), "mount mkdir");
	check(!call(NR_MOUNT, (word)"none", (word)"/tmp/open-close/mount", (word)"tmpfs", 0, 0), "tmpfs mount");
	create("/tmp/open-close/mount/file", 't');
	unsigned repetition;
	for (repetition = 0; repetition < OPEN_CLOSE_REPEATS; ++repetition) {
		benchmark("normal", "/tmp/open-close/file");
		benchmark("symlink", "/tmp/open-close/link");
		benchmark("mount", "/tmp/open-close/mount/file");
	}
#ifdef OPEN_CLOSE_BENCH_ONLY
	print("OPEN_CLOSE_BENCHMARK_PASS\n");
	finish(0);
#endif
	check(!call(NR_CHDIR, (word)"/tmp/open-close", 0, 0, 0, 0), "chdir for relative opens");
	contents("file", 'a');
	contents("./link", 'a');
	close_file(open_file("#! /tmp/open-close/file ignored", 0));
	check(!call(NR_CHDIR, (word)"/", 0, 0, 0, 0), "restore cwd");
	contents("/tmp/open-close/link", 'a');
	close_file(open_file("/tmp/open-close/link", 1));
	close_file(open_file("/tmp/open-close/link", O_PATH | O_NOFOLLOW));
	check(open_file("/tmp/open-close/file", O_DIRECTORY) < 0, "file is not directory");
	close_file(open_file("/tmp/open-close", O_DIRECTORY));
	check(!call(NR_SYMLINK, (word)"mount/file", (word)"/tmp/open-close/cross", 0, 0, 0), "cross-mount symlink");
	contents("/tmp/open-close/cross", 't');
	check(!call(NR_SYMLINK, (word)"link", (word)"/tmp/open-close/chain", 0, 0, 0), "symlink chain");
	contents("/tmp/open-close/chain", 'a');
	/* A non-inline target exercises the handle reader's block-backed case. */
	const char *long_target =
		"/tmp/open-close/././././././././././././././././././././././././file";
	check(!call(NR_SYMLINK,
		    (word)long_target,
		    (word)"/tmp/open-close/long-link", 0, 0, 0), "long symlink");
	char target_text[128] = {0};
	result target_length = call(NR_READLINK, (word)"/tmp/open-close/long-link",
				    (word)target_text, 127, 0, 0);
	check(target_length > 60 && target_length < 128, "long readlink length");
	unsigned target_index;
	for (target_index = 0; long_target[target_index]; ++target_index)
		check(target_text[target_index] == long_target[target_index], "long readlink contents");
	check(target_index == (word)target_length, "exact readlink length");
	contents("/tmp/open-close/long-link", 'a');
	/* Exactly 60 bytes must use the block-backed reader, not inline text. */
	char boundary_target[61];
	const char *prefix = "/tmp/open-close/";
	unsigned boundary_length = 0;
	while (prefix[boundary_length]) {
		boundary_target[boundary_length] = prefix[boundary_length];
		boundary_length++;
	}
	unsigned dot;
	for (dot = 0; dot < 20; ++dot) {
		boundary_target[boundary_length++] = '.';
		boundary_target[boundary_length++] = '/';
	}
	const char *filename = "file";
	for (dot = 0; filename[dot]; ++dot)
		boundary_target[boundary_length++] = filename[dot];
	check(boundary_length == 60, "boundary target length");
	boundary_target[boundary_length] = 0;
	check(!call(NR_SYMLINK, (word)boundary_target,
		    (word)"/tmp/open-close/boundary-link", 0, 0, 0), "boundary symlink");
	contents("/tmp/open-close/boundary-link", 'a');
	check(!call(NR_SYMLINK, (word)"missing", (word)"/tmp/open-close/dangling", 0, 0, 0), "dangling symlink");
	check(open_file("/tmp/open-close/dangling", 0) < 0, "dangling open fails");
	check(!call(NR_MKDIR, (word)"/tmp/open-close/parent", 0700, 0, 0, 0), "nested mkdir");
	create("/tmp/open-close/parent/child", 'n');
	contents("/tmp/open-close/parent/child", 'n');
	contents("/tmp/open-close/parent/child", 'n');
	check(!call(NR_RENAME, (word)"/tmp/open-close/parent",
		    (word)"/tmp/open-close/moved-parent", 0, 0, 0), "parent rename");
	check(open_file("/tmp/open-close/parent/child", 0) < 0, "cached ancestor rename");
	contents("/tmp/open-close/moved-parent/child", 'n');
	check(!call(NR_SYMLINK, (word)"child",
		    (word)"/tmp/open-close/moved-parent/indirect", 0, 0, 0), "nested relative link");
	check(!call(NR_SYMLINK, (word)"moved-parent",
		    (word)"/tmp/open-close/dir-alias", 0, 0, 0), "directory symlink");
	contents("/tmp/open-close/dir-alias/indirect", 'n');
	check(!call(NR_SYMLINK, (word)"loop-b",
		    (word)"/tmp/open-close/loop-a", 0, 0, 0), "loop first link");
	check(!call(NR_SYMLINK, (word)"loop-a",
		    (word)"/tmp/open-close/loop-b", 0, 0, 0), "loop second link");
	check(open_file("/tmp/open-close/loop-a", 0) < 0, "bounded symlink loop");
	check(!call(NR_MKDIR, (word)"/tmp/open-close/parent", 0700, 0, 0, 0), "recreate parent");
	create("/tmp/open-close/parent/child", 'r');
	contents("/tmp/open-close/parent/child", 'r');

	/* Cached lookups must observe rename, replacement, unlink, and inode reuse. */
	create("/tmp/open-close/replacement", 'b');
	check(!call(NR_RENAME, (word)"/tmp/open-close/replacement", (word)"/tmp/open-close/file", 0, 0, 0), "rename replacement");
	contents("/tmp/open-close/file", 'b');
	contents("/tmp/open-close/link", 'b');
	result held = open_file("/tmp/open-close/file", 0);
	result held2 = open_file("/tmp/open-close/file", 0);
	check(held >= 0 && held2 >= 0, "independent opens before unlink");
	check(!call(NR_LINK, (word)"/tmp/open-close/file", (word)"/tmp/open-close/hard", 0, 0, 0), "hard link");
	check(!call(NR_UNLINK, (word)"/tmp/open-close/file", 0, 0, 0, 0), "unlink first name");
	contents("/tmp/open-close/hard", 'b');
	check(!call(NR_UNLINK, (word)"/tmp/open-close/hard", 0, 0, 0, 0), "unlink last name");
	check(open_file("/tmp/open-close/file", 0) < 0, "cached removed path fails");
	char value = 0;
	check(call(NR_READ, held, (word)&value, 1, 0, 0) == 1 && value == 'b', "unlinked fd readable");
	close_file(held);
	value = 0;
	check(call(NR_READ, held2, (word)&value, 1, 0, 0) == 1 && value == 'b', "orphan retained until last close");
	close_file(held2);
	create("/tmp/open-close/file", 'c');
	contents("/tmp/open-close/link", 'c');
	check(!call(NR_UNLINK, (word)"/tmp/open-close/link", 0, 0, 0, 0), "remove cached symlink");
	check(!call(NR_SYMLINK, (word)"mount/file", (word)"/tmp/open-close/link", 0, 0, 0), "replace symlink target");
	contents("/tmp/open-close/link", 't');
	watch_open_close("/tmp/open-close/file");
	watch_open_close("/tmp/open-close/mount/file");
	unlinked_duplicate("/tmp/open-close/mount/file", 't');
	create("/tmp/open-close/mount/file", 't');
	create("/tmp/open-close/dup-file", 'd');
	unlinked_duplicate("/tmp/open-close/dup-file", 'd');
	close_file(open_file("/tmp/open-close/file", 1 | O_TRUNC));
	result empty = open_file("/tmp/open-close/file", 0);
	check(empty >= 0 && call(NR_READ, empty, (word)&value, 1, 0, 0) == 0, "cached truncate");
	close_file(empty);

	/* Metadata is not cached: non-root opens must see chmod immediately. */
	check(!call(NR_CHMOD, (word)"/tmp/open-close", 0755, 0, 0, 0), "directory chmod");
	check(!call(NR_CHMOD, (word)"/tmp/open-close/file", 0000, 0, 0, 0), "file chmod denied");
	close_file(open_file("/tmp/open-close/file", 0));
	result child = call(NR_FORK, 0, 0, 0, 0, 0);
	check(child >= 0, "fork permission check");
	if (!child) {
		check(!call(NR_SETUID, 1000, 0, 0, 0, 0), "drop uid");
		result fd = open_file("/tmp/open-close/file", 0);
		call(NR_EXIT, fd == -13 ? 0 : 1, 0, 0, 0, 0);
		for (;;) {}
	}
	int status = -1;
	check(call(NR_WAIT, child, (word)&status, 0, 0, 0) == child && status == 0,
	      "non-root cached open respects permissions");
	print("OPEN_CLOSE_PROBE_PASS\n");
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
