#!/bin/sh
# Validate rlimit output protection and writable copy-on-write pages.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-prlimit_protection.XXXXXX)
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

int main(void)
{
	size_t page = getpagesize(), i;
	unsigned char *p = mmap(0, page, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	pid_t pid;
	CHECK(p != MAP_FAILED);
	memset(p, 'Q', page);
	CHECK(!mprotect(p, page, PROT_READ));
	CHECK(syscall(340, 0, 6, 0, p) == -1 && errno == EFAULT);
	for (i = 0; i < page; i++)
		CHECK(p[i] == 'Q');
	CHECK(!mprotect(p, page, 3));
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		CHECK(!syscall(340, 0, 6, 0, p));
		CHECK(memcmp(p, "QQQQQQQQQQQQQQQQ", 16));
		_exit(0);
	}
	child_ok(pid);
	for (i = 0; i < page; i++)
		CHECK(p[i] == 'Q');
	CHECK(!syscall(340, 0, 6, 0, p));
	CHECK(!munmap(p, page));
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
