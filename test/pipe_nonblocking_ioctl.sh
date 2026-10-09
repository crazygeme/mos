#!/bin/sh
# Validate pipe nonblocking ioctl and descriptor metadata isolation.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-pipe_nonblocking_ioctl.XXXXXX)
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

int main(void)
{
	int p[2], one = 1, zero = 0, n, i;
	char path[64], a[128], b[128], data[4];
	ssize_t al, bl;
	struct stat st;
	CHECK(!pipe(p));
	snprintf(path, sizeof(path), "/proc/self/fd/%d", p[0]);
	CHECK((al = readlink(path, a, sizeof(a))) > 0);
	CHECK(!stat(path, &st));
	CHECK((bl = readlink(path, b, sizeof(b))) == al && !memcmp(a, b, al));
	for (i = 0; i < 2; i++) {
		CHECK(!ioctl(p[i], FIONBIO, &one));
		CHECK(fcntl(p[i], F_GETFL) & O_NONBLOCK);
	}
	CHECK(read(p[0], data, 1) == -1 && errno == EAGAIN);
	CHECK(write(p[1], "pipe", 4) == 4);
	CHECK(!ioctl(p[0], FIONREAD, &n) && n == 4);
	CHECK(read(p[0], data, 4) == 4 && !memcmp(data, "pipe", 4));
	CHECK(!ioctl(p[0], FIONBIO, &zero));
	CHECK(!(fcntl(p[0], F_GETFL) & O_NONBLOCK));
	close(p[0]);
	close(p[1]);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
