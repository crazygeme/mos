#!/bin/sh
# Validate process memory reporting across reservation, faults, and unmap.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_memory.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC O_CLOEXEC
#define SOCK_NONBLOCK O_NONBLOCK
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif
#define CHECK(x)                                                            \
	do {                                                                \
		if (!(x)) {                                                 \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, \
				__LINE__, #x, errno);                       \
			exit(1);                                            \
		}                                                           \
	} while (0)
static void child_ok(pid_t pid)
{
	int status;
	CHECK(pid > 0);
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

#include <dirent.h>

static long field(const char *path, const char *key)
{
	FILE *f = fopen(path, "r");
	char line[1024], name[80];
	long n;
	CHECK(f);
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%79[^:]: %ld", name, &n) == 2 &&
		    !strcmp(name, key)) {
			fclose(f);
			return n;
		}
	}
	fclose(f);
	CHECK(0);
	return -1;
}

struct snapshot {
	long v, r;
};
static pid_t pid;
static struct snapshot snap(void)
{
	char path[128], line[4096], *p, *end;
	long counts[3], v, r, shared, values[22];
	int i;
	FILE *f;
	struct snapshot s;
	snprintf(path, sizeof(path), "/proc/%d/statm", pid);
	CHECK((f = fopen(path, "r")));
	CHECK(fscanf(f, "%ld %ld %ld", &counts[0], &counts[1], &counts[2]) ==
	      3);
	fclose(f);
	v = counts[0] * getpagesize();
	r = counts[1] * getpagesize();
	shared = counts[2] * getpagesize();
	CHECK(r > 0 && r <= v && shared <= r);
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	CHECK(field(path, "VmSize") * 1024 == v &&
	      field(path, "VmRSS") * 1024 == r &&
	      field(path, "VmStk") * 1024 <= v);
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	CHECK((f = fopen(path, "r")));
	CHECK(fgets(line, sizeof(line), f));
	fclose(f);
	p = strrchr(line, ')');
	CHECK(p);
	p += 2;
	CHECK(*p);
	p += 2;
	for (i = 1; i < 22; i++) {
		values[i] = strtol(p, &end, 10);
		CHECK(end != p);
		p = end;
	}
	CHECK(values[20] == v && values[21] * getpagesize() == r);
	s.v = v;
	s.r = r;
	return s;
}
int main(void)
{
	int commands[2], ready[2];
	char c;
	unsigned char *mapping = 0;
	size_t i;
	struct snapshot baseline, reserved, faulted, unmapped;
	alarm(10);
	CHECK(!pipe(commands) && !pipe(ready));
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		close(commands[1]);
		close(ready[0]);
		CHECK(write(ready[1], "R", 1) == 1);
		while (read(commands[0], &c, 1) == 1) {
			if (c == 'M') {
				mapping = mmap(0, 64 * 1024 * 1024, 3,
					       MAP_PRIVATE | MAP_ANONYMOUS, -1,
					       0);
				CHECK(mapping != MAP_FAILED);
			}
			if (c == 'T')
				for (i = 0; i < 4 * 1024 * 1024;
				     i += getpagesize())
					mapping[i] = 1;
			if (c == 'U')
				CHECK(!munmap(mapping, 64 * 1024 * 1024));
			CHECK(write(ready[1], "R", 1) == 1);
		}
		_exit(0);
	}
	close(commands[0]);
	close(ready[1]);
	CHECK(read(ready[0], &c, 1) == 1);
	baseline = snap();
	CHECK(write(commands[1], "M", 1) == 1 && read(ready[0], &c, 1) == 1);
	reserved = snap();
	CHECK(labs(reserved.v - baseline.v - 64 * 1024 * 1024) <= 1024 * 1024 &&
	      labs(reserved.r - baseline.r) <= 1024 * 1024);
	CHECK(write(commands[1], "T", 1) == 1 && read(ready[0], &c, 1) == 1);
	faulted = snap();
	CHECK(labs(faulted.v - reserved.v) <= 1024 * 1024 &&
	      labs(faulted.r - reserved.r - 4 * 1024 * 1024) <= 1024 * 1024);
	CHECK(write(commands[1], "U", 1) == 1 && read(ready[0], &c, 1) == 1);
	unmapped = snap();
	CHECK(labs(unmapped.v - baseline.v) <= 1024 * 1024 &&
	      labs(unmapped.r - baseline.r) <= 1024 * 1024);
	close(commands[1]);
	close(ready[0]);
	child_ok(pid);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
