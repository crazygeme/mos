#!/bin/sh
# Validate large socket vector I/O without payload-sized kernel buffers.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-socket_vector_large.XXXXXX)
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

static int s[2];
static unsigned char *expected;
static void *reader(void *unused)
{
	unsigned char *b = malloc(131072);
	struct iovec v[2];
	size_t total = 0;
	ssize_t n;
	CHECK(b);
	while (total < 4 * 1024 * 1024) {
		v[0].iov_base = b;
		v[0].iov_len = 65536;
		v[1].iov_base = b + 65536;
		v[1].iov_len = 65536;
		n = readv(s[1], v, 2);
		CHECK(n > 0 && !memcmp(b, expected + total, n));
		total += n;
	}
	free(b);
	return 0;
}
int main(void)
{
	pthread_t t;
	struct iovec v[4];
	size_t done = 0, n, i, left;
	ssize_t r;
	int count;
	alarm(20);
	expected = malloc(4 * 1024 * 1024);
	CHECK(expected);
	for (i = 0; i < 4; i++)
		memset(expected + i * 1024 * 1024, i, 1024 * 1024);
	CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
	CHECK(!pthread_create(&t, 0, reader, 0));
	while (done < 4 * 1024 * 1024) {
		count = 0;
		n = done;
		while (n < 4 * 1024 * 1024) {
			left = 1024 * 1024 - n % (1024 * 1024);
			v[count].iov_base = expected + n;
			v[count++].iov_len = left;
			n += left;
		}
		r = writev(s[0], v, count);
		CHECK(r > 0);
		done += r;
	}
	CHECK(!pthread_join(t, 0));
	free(expected);
	close(s[0]);
	close(s[1]);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
