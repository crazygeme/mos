#!/bin/sh
set -e

BASE=/root/tests/posix_llseek
mkdir -p "$BASE"
cat > "$BASE/llseek.c" <<'EOF'
#define _LARGEFILE64_SOURCE
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "llseek: %s\n", what);
		exit(1);
	}
}

int main(void)
{
	static const unsigned long long positions[] = {
		0, 4096, 0x7ffff000ULL, 0x80002000ULL, 0xfffff000ULL,
		0x100000000ULL
	};
	struct {
		unsigned long long position;
		unsigned long long guard;
	} result;
	int fd = open("/dev/mem", O_RDONLY);
	unsigned i;
	struct sysinfo info;
	FILE *meminfo;
	char line[128];
	unsigned long long total_kb = 0;

	check(sizeof(void *) == 4, "i386 syscall ABI");
	check(fd >= 0, "open physical memory");
	for (i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
		result.position = ~positions[i];
		result.guard = 0x123456789abcdef0ULL;
		check(syscall(__NR__llseek, fd, (unsigned)(positions[i] >> 32),
		              (unsigned)positions[i], &result.position, SEEK_SET) == 0,
		      "raw syscall success status");
		check(result.position == positions[i], "full result position");
		check(result.guard == 0x123456789abcdef0ULL, "result buffer bounds");
		check(lseek64(fd, 0, SEEK_CUR) == (off64_t)positions[i],
		      "libc current position");
		check(lseek64(fd, (off64_t)positions[i], SEEK_SET) ==
		      (off64_t)positions[i], "libc absolute position");
	}
	check(syscall(__NR__llseek, fd, ~0U, ~0U, &result.position,
	              SEEK_SET) == -1, "negative seek fails");
	check(lseek64(fd, 0, SEEK_CUR) == (off64_t)positions[i - 1],
	      "failed seek preserves position");
	check(close(fd) == 0, "close physical memory");
	check(sysinfo(&info) == 0 && info.mem_unit != 0, "memory units");
	meminfo = fopen("/proc/meminfo", "r");
	check(meminfo != NULL, "open memory statistics");
	while (fgets(line, sizeof(line), meminfo))
		if (sscanf(line, "MemTotal: %llu kB", &total_kb) == 1)
			break;
	check(fclose(meminfo) == 0, "close memory statistics");
	check(total_kb > 0 && (unsigned long long)info.totalram * info.mem_unit
	      == total_kb * 1024, "sysinfo represents all RAM without overflow");
	puts("llseek: PASS");
	return 0;
}
EOF
gcc -Wall -Werror -o "$BASE/llseek" "$BASE/llseek.c"
"$BASE/llseek"
