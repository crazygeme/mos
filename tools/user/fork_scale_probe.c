/* Isolate fork/COW cost at a fixed resident size with varying VMA counts. */
typedef unsigned long word;
typedef long result;
#ifdef __x86_64__
#define NR_BRK 12
#define NR_MAP 9
#define NR_UNMAP 11
#define NR_WRITE 1
#define NR_MOUNT 165
#define NR_OPEN 2
#define NR_CLOSE 3
#define NR_FORK 57
#define NR_EXIT 60
#define NR_WAIT 61
#define NR_TIME 96
#define NR_IOPL 172
static result call(word n, word a, word b, word c, word d, word e)
{
	register word r10 asm("r10") = d, r8 asm("r8") = e, r9 asm("r9") = 0;
	result ret;
	asm volatile("syscall"
		     : "=a"(ret)
		     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
		       "r"(r9)
		     : "rcx", "r11", "memory", "cc");
	return ret;
}
#else
#define NR_BRK 45
#define NR_MAP 192
#define NR_UNMAP 91
#define NR_WRITE 4
#define NR_MOUNT 21
#define NR_OPEN 5
#define NR_CLOSE 6
#define NR_FORK 2
#define NR_EXIT 1
#define NR_WAIT 114
#define NR_TIME 78
#define NR_IOPL 110
__attribute__((naked, noinline, noclone)) static result
call(word n, word a, word b, word c, word d, word e)
{
	asm volatile(
		"push %ebp; push %edi; push %esi; push %ebx;"
		"mov 20(%esp),%eax; mov 24(%esp),%ebx;"
		"mov 28(%esp),%ecx; mov 32(%esp),%edx;"
		"mov 36(%esp),%esi; mov 40(%esp),%edi;"
		"xor %ebp,%ebp; int $0x80; pop %ebx; pop %esi; pop %edi; pop %ebp; ret");
}
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
		print("FAIL ");
		print(name);
		print("\n");
		finish(1);
	}
}

struct timeval {
	word sec, us;
};
static word now(void)
{
	struct timeval t;
	check(call(NR_TIME, (word)&t, 0, 0, 0, 0) == 0, "clock");
	return t.sec * 1000000 + t.us;
}

#ifdef FORK_SCALE_KERNEL_TESTS
static void kernel_tests(void)
{
	check(call(NR_MOUNT, (word) "proc", (word) "/proc", (word) "proc", 0,
		   0) == 0,
	      "mount proc");
	result fd = call(NR_OPEN, (word) "/proc/tests/.runner", 1, 0, 0, 0);
	check(fd >= 0, "open test runner");
	check(call(NR_WRITE, fd, (word) "mmap", 4, 0, 0) == 4,
	      "run mmap tests");
	check(call(NR_WRITE, fd, (word) "malloc", 6, 0, 0) == 6,
	      "run malloc tests");
	check(call(NR_CLOSE, fd, 0, 0, 0, 0) == 0, "close runner");
}
#endif
static void bench(unsigned regions, unsigned pages, int heap)
{
	word base = heap ? call(NR_BRK, 0, 0, 0, 0, 0) : 0x20000000;
	base = (base + 4095) & ~4095UL;
	if (heap)
		check(call(NR_BRK, base, 0, 0, 0, 0) == base, "align brk");
	for (unsigned i = 0; i < regions; i++) {
		word start = base + i * (pages / regions) * 4096,
		     end = start + (pages / regions) * 4096;
		if (heap)
			check(call(NR_BRK, end, 0, 0, 0, 0) == end,
			      "brk growth");
		else
			check(call(NR_MAP, start, end - start, 3, 0x32, -1) ==
				      start,
			      "mapping");
	}
	for (unsigned i = 0; i < pages; i++)
		*(volatile word *)(base + i * 4096) = i + 1;
	word elapsed = now();
	for (unsigned i = 0; i < 1000; i++) {
		result child = call(NR_FORK, 0, 0, 0, 0, 0);
		check(child >= 0, "fork");
		if (!child) {
			for (unsigned j = 0; j < pages; j++)
				if (*(volatile word *)(base + j * 4096) !=
				    j + 1)
					call(NR_EXIT, 1, 0, 0, 0, 0);
			*(volatile word *)(base + (pages - 1) * 4096) = 0;
			call(NR_EXIT, 0, 0, 0, 0, 0);
			for (;;)
				;
		}
		int status = 0;
		check(call(NR_WAIT, child, (word)&status, 0, 0, 0) == child &&
			      status == 0,
		      "wait/COW data");
		check(*(volatile word *)(base + (pages - 1) * 4096) == pages,
		      "parent COW data");
	}
	elapsed = now() - elapsed;
	print("BENCH ");
	print(heap ? "brk" : "mmap");
	print(" regions=");
	decimal(regions);
	print(" pages=");
	decimal(pages);
	print(" forks=1000 elapsed_us=");
	decimal(elapsed);
	print("\n");
	if (heap)
		check(call(NR_BRK, base, 0, 0, 0, 0) == base, "brk shrink");
	else
		check(call(NR_UNMAP, base, pages * 4096, 0, 0, 0) == 0,
		      "unmap");
}
void entry(void)
{
	check(call(NR_IOPL, 3, 0, 0, 0, 0) == 0, "iopl");
#ifdef FORK_SCALE_KERNEL_TESTS
	kernel_tests();
#endif
	bench(1, 256, 0);
	bench(8, 256, 0);
	bench(64, 256, 0);
	bench(1, 256, 1);
	bench(8, 256, 1);
	bench(64, 256, 1);
	print("FORK_SCALE_PASS\n");
	finish(0);
}
void _start(void)
{
	entry();
}
