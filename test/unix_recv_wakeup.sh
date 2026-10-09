#!/bin/sh
# Check that Unix recv and recvfrom wake a sender blocked in poll.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_recv_wakeup.XXXXXX)
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
	int op, s[2], p[2], i;
	size_t filled, n, received;
	ssize_t r;
	unsigned char b[65536];
	struct pollfd poller;
	pid_t child;
	alarm(10);
	for (i = 0; i < sizeof(b); i++)
		b[i] = i % 256;
	for (op = 0; op < 2; op++) {
		CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
		CHECK(!fcntl(s[0], F_SETFL, O_NONBLOCK));
		filled = 0;
		while ((r = send(s[0], b, sizeof(b), 0)) > 0)
			filled += r;
		CHECK(r == -1 && errno == EAGAIN && filled);
		poller.fd = s[0];
		poller.events = POLLOUT;
		CHECK(!poll(&poller, 1, 0));
		CHECK(!pipe(p));
		child = fork();
		CHECK(child >= 0);
		if (!child) {
			close(s[1]);
			close(p[0]);
			CHECK(write(p[1], "ready", 5) == 5);
			CHECK(poll(&poller, 1, 2000) == 1 &&
			      (poller.revents & POLLOUT));
			_exit(0);
		}
		close(p[1]);
		CHECK(read(p[0], b, 5) == 5);
		usleep(50000);
		received = 0;
		while (received < filled) {
			n = filled - received;
			if (n > sizeof(b))
				n = sizeof(b);
			r = op ? recvfrom(s[1], b, n, 0, 0, 0) :
				 recv(s[1], b, n, 0);
			CHECK(r > 0);
			for (i = 0; i < r; i++)
				CHECK(b[i] == (received + i) % 256);
			received += r;
		}
		child_ok(child);
		close(p[0]);
		close(s[0]);
		close(s[1]);
		for (i = 0; i < sizeof(b); i++)
			b[i] = i % 256;
	}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
