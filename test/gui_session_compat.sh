#!/bin/sh
# Validate GUI session socketpair and futex interfaces inside a MOS guest.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-gui_session_compat.XXXXXX)
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

struct ts64 {
	int64_t sec, nsec;
};
static int word;
/* glibc 2.3's syscall() forwards only five arguments; bitsets use EBP. */
extern long raw_futex(int *, int, int, const struct ts64 *, int *, unsigned);
__asm__(".text\n"
	".globl raw_futex\n"
	"raw_futex:\n"
	"pushl %ebp\n pushl %ebx\n pushl %esi\n pushl %edi\n"
	"movl 20(%esp), %ebx\n movl 24(%esp), %ecx\n"
	"movl 28(%esp), %edx\n movl 32(%esp), %esi\n"
	"movl 36(%esp), %edi\n movl 40(%esp), %ebp\n"
	"movl $422, %eax\n int $0x80\n"
	"popl %edi\n popl %esi\n popl %ebx\n popl %ebp\n ret\n");
static long futex(int op, int value, struct ts64 *time, unsigned mask)
{
	long result = raw_futex(&word, op, value, time, NULL, mask);
	if (result < 0) {
		errno = -result;
		return -1;
	}
	return result;
}
static void *waiter(void *unused)
{
	struct ts64 now;
	CHECK(!syscall(403, 1, &now));
	now.sec += 2;
	CHECK(!futex(137, 0, &now, 2));
	return 0;
}
int main(void)
{
	int i, j, s[2],
		flags[] = { 0, SOCK_CLOEXEC, SOCK_NONBLOCK,
			    SOCK_CLOEXEC | SOCK_NONBLOCK };
	char b[8];
	struct ts64 timeout;
	pthread_t t;
	long r;
	alarm(10);
	for (i = 0; i < 4; i++) {
		CHECK(!socketpair(AF_UNIX, SOCK_STREAM | flags[i], 0, s));
		for (j = 0; j < 2; j++) {
			CHECK(!!(fcntl(s[j], F_GETFD) & FD_CLOEXEC) ==
			      !!(flags[i] & SOCK_CLOEXEC));
			CHECK(!!(fcntl(s[j], F_GETFL) & O_NONBLOCK) ==
			      !!(flags[i] & SOCK_NONBLOCK));
		}
		CHECK(write(s[0], "request", 7) == 7 && read(s[1], b, 7) == 7 &&
		      !memcmp(b, "request", 7));
		CHECK(write(s[1], "reply", 5) == 5 && read(s[0], b, 5) == 5 &&
		      !memcmp(b, "reply", 5));
		if (flags[i] & SOCK_NONBLOCK)
			CHECK(read(s[0], b, 1) == -1 && errno == EAGAIN);
		close(s[0]);
		close(s[1]);
	}
	CHECK(futex(128, 1, 0, ~0U) == -1 && errno == EAGAIN);
	timeout.sec = timeout.nsec = 0;
	CHECK(futex(128, 0, &timeout, ~0U) == -1 && errno == ETIMEDOUT);
	timeout.nsec = 1000000000;
	CHECK(futex(128, 0, &timeout, ~0U) == -1 && errno == EINVAL);
	CHECK(futex(137, 0, 0, 0) == -1 && errno == EINVAL);
	timeout.nsec = 0;
	CHECK(futex(137, 0, &timeout, ~0U) == -1 && errno == ETIMEDOUT);
	CHECK(futex(137 | 256, 0, &timeout, ~0U) == -1 && errno == ETIMEDOUT);
	CHECK(!pthread_create(&t, 0, waiter, 0));
	CHECK(futex(138, 1, 0, 1) == 0);
	for (i = 0; i < 100; i++) {
		r = futex(138, 1, 0, 2);
		CHECK(r >= 0);
		if (r == 1)
			break;
		usleep(10000);
	}
	CHECK(i < 100);
	CHECK(!pthread_join(t, 0));
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
