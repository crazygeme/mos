/* Freestanding AMD64 ABI and SMP regression program; no libc dependency. */
typedef unsigned long u64;
typedef long s64;
static s64 call6(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
	register u64 r10 asm("r10") = d, r8 asm("r8") = e, r9 asm("r9") = f;
	s64 result;
	asm volatile("syscall"
		     : "=a"(result)
		     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
		       "r"(r9)
		     : "rcx", "r11", "memory");
	return result;
}
static __attribute__((noreturn)) void finish(int status)
{
	call6(60, status, 0, 0, 0, 0, 0);
	for (;;)
		asm volatile("ud2");
}
static void require(int truth, int status)
{
	if (!truth)
		finish(status);
}
static void probe_physical_offsets(void)
{
	const char path[] = "/dev/mem";
	s64 fd = call6(2, (u64)path, 0, 0, 0, 0, 0);
	require(fd >= 0, 32);
	/* Seek without dereferencing: this also works on a 4 GiB VM. */
	require(call6(8, fd, 0x100002000UL, 0, 0, 0, 0) == 0x100002000L,
		33);
	s64 map = call6(9, 0, 12288, 1, 1, fd, 0x100000000UL);
	require(map >= 0x100000000L, 34);
	/* Check that splitting the VMA retains all physical offset bits. */
	require(!call6(10, map + 4096, 4096, 0, 0, 0, 0), 35);
	require(!call6(10, map + 4096, 4096, 1, 0, 0, 0), 36);
	require(!call6(11, map, 4096, 0, 0, 0, 0), 37);
	require(!call6(11, map + 4096, 8192, 0, 0, 0, 0), 38);
	require(!call6(3, fd, 0, 0, 0, 0, 0), 39);
}
static void probe_large_shared_mapping(void)
{
	u64 length = 0x100004000UL;
	s64 map = call6(9, 0, length, 3, 0x21, (u64)-1, 0);
	require(map >= 0x100000000L, 40);
	volatile u64 *low = (void *)map;
	volatile u64 *high = (void *)(map + 0x100000000UL);
	*low = 0x123456789abcdef0UL;
	*high = 0xfedcba9876543210UL;
	require(*low == 0x123456789abcdef0UL &&
		*high == 0xfedcba9876543210UL, 41);
	require(!call6(10, (u64)high, 4096, 1, 0, 0, 0), 42);
	require(*low != *high, 43);
	require(!call6(11, map, 0x100000000UL, 0, 0, 0, 0), 44);
	require(*high == 0xfedcba9876543210UL, 45);
	require(!call6(11, (u64)high, 16384, 0, 0, 0, 0), 46);
}
static void probe_ifconf(void)
{
	struct request {
		char name[16];
		unsigned short family;
		unsigned char rest[22];
	};
	struct config {
		int len;
		u64 buffer;
	};
	_Static_assert(sizeof(struct request) == 40, "AMD64 ifreq");
	_Static_assert(sizeof(struct config) == 16, "AMD64 ifconf");
	s64 page = call6(9, 0, 4096, 3, 0x22, (u64)-1, 0);
	require(page >= 0x100000000L, 21);
	unsigned char *buffer = (void *)page;
	for (unsigned i = 0; i < 4096; i++)
		buffer[i] = 0xa5;
	s64 fd = call6(41, 2, 2, 0, 0, 0, 0);
	require(fd >= 0, 22);
	struct config config = { 4080, page };
	require(!call6(16, fd, 0x8912, (u64)&config, 0, 0, 0), 23);
	require(config.buffer == (u64)page && config.len > 0 &&
			config.len <= 4080 && config.len % 40 == 0,
		24);
	struct request *requests = (void *)page;
	for (unsigned i = 0; i < (unsigned)config.len / 40; i++)
		require(requests[i].name[0] && requests[i].family == 2, 25);
	for (unsigned i = config.len; i < 4096; i++)
		require(buffer[i] == 0xa5, 26);
	struct config query = { 0, 0 };
	require(!call6(16, fd, 0x8912, (u64)&query, 0, 0, 0) &&
			query.len == config.len,
		27);
	for (unsigned i = 0; i < 4096; i++)
		buffer[i] = 0xa5;
	config.len = 39;
	require(!call6(16, fd, 0x8912, (u64)&config, 0, 0, 0) && !config.len,
		28);
	for (unsigned i = 0; i < 4096; i++)
		require(buffer[i] == 0xa5, 29);
	require(!call6(3, fd, 0, 0, 0, 0, 0), 30);
	require(!call6(11, page, 4096, 0, 0, 0, 0), 31);
}
static volatile int delivered;
static void handler(int sig)
{
	if (sig == 10)
		delivered++;
}
__attribute__((naked)) static void restorer(void)
{
	asm volatile("mov $15,%rax; syscall; ud2");
}
static volatile u64 *shared_data;
static volatile int shared_ready;
__attribute__((used, noreturn, noinline)) static void shared_worker(void)
{
	for (;;) {
		*shared_data = 0xabcdef;
		shared_ready = 1;
	}
}
__attribute__((naked)) static s64 clone_shared(u64 flags, u64 stack)
{
	asm volatile(
		"xor %edx,%edx; xor %r10d,%r10d; xor %r8d,%r8d; mov $56,%eax; syscall; test %rax,%rax; jnz 1f; xor %ebp,%ebp; call shared_worker; ud2; 1: ret");
}
__attribute__((used, noinline)) static int probe_main(void)
{
	probe_ifconf();
	probe_physical_offsets();
	probe_large_shared_mapping();
	s64 page = call6(9, 0, 4096, 3, 0x22, (u64)-1, 0);
	require(page >= 0x100000000L, 1);
	volatile u64 *data = (void *)page;
	*data = 0x123456789abcdef0UL;
	require(!call6(158, 0x1002, page, 0, 0, 0, 0), 2);
	require(!call6(158, 0x1001, page, 0, 0, 0, 0), 3);
	u64 fs, gs;
	asm volatile("movq %%fs:0,%0; movq %%gs:0,%1" : "=r"(fs), "=r"(gs));
	require(fs == *data && gs == *data, 4);
	struct {
		u64 handler, flags, restorer, mask;
	} action = { (u64)handler, 0x04000000, (u64)restorer, 0 };
	require(!call6(13, 10, (u64)&action, 0, 8, 0, 0), 5);
	s64 pid = call6(39, 0, 0, 0, 0, 0, 0);
	require(!call6(62, pid, 10, 0, 0, 0, 0) && delivered == 1, 6);
	s64 children[8];
	for (unsigned i = 0; i < 8; i++) {
		s64 child = call6(57, 0, 0, 0, 0, 0, 0);
		require(child >= 0, 7);
		if (!child) {
			require(*data == 0x123456789abcdef0UL, 8);
			*data = 43 + i;
			for (unsigned j = 0; j < 32; j++) {
				call6(24, 0, 0, 0, 0, 0, 0);
				asm volatile("movq %%fs:0,%0; movq %%gs:0,%1"
					     : "=r"(fs), "=r"(gs));
				require(fs == 43 + i && gs == 43 + i, 16);
			}
			finish(0);
		}
		children[i] = child;
	}
	for (unsigned i = 0; i < 8; i++) {
		int status = -1;
		require(call6(61, children[i], (u64)&status, 0, 0, 0, 0) ==
					children[i] &&
				status == 0,
			9);
		require(*data == 0x123456789abcdef0UL, 10);
	}
	/* A separate process shares this VM and continuously writes the page.
  * Removing write permission must invalidate its cached translation too. */
	s64 stack = call6(9, 0, 16384, 3, 0x22, (u64)-1, 0);
	require(stack >= 0x100000000L, 17);
	shared_data = data;
	shared_ready = 0;
	s64 writer = clone_shared(0x111, stack + 16384);
	require(writer > 0, 18);
	while (!shared_ready)
		call6(24, 0, 0, 0, 0, 0, 0);
	for (unsigned i = 0; i < 16; i++)
		call6(24, 0, 0, 0, 0, 0, 0);
	require(!call6(10, page, 4096, 1, 0, 0, 0), 11);
	int writer_status = 0;
	require(call6(61, writer, (u64)&writer_status, 0, 0, 0, 0) == writer &&
			(writer_status & 127) == 11,
		19);
	require(!call6(11, stack, 16384, 0, 0, 0, 0), 20);
	require(!call6(158, 0x1002, 0, 0, 0, 0, 0), 12);
	require(!call6(158, 0x1001, 0, 0, 0, 0, 0), 13);
	require(!call6(11, page, 4096, 0, 0, 0, 0), 14);
	const char message[] = "AMD64 ABI smoke: PASS\n";
	require(call6(1, 1, (u64)message, sizeof(message) - 1, 0, 0, 0) ==
			sizeof(message) - 1,
		15);
	return 0;
}
__attribute__((naked, noreturn)) void _start(void)
{
	asm volatile(
		"xor %rbp,%rbp; andq $-16,%rsp; call probe_main; mov %eax,%edi; mov $60,%eax; syscall; ud2");
}
