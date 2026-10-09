#!/bin/sh
# Exercise process exit with active threads and blocked I/O waiters.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-exit_io_wait.XXXXXX)
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

static int ready, endpoint, operation;
static void *worker(void *unused)
{
	char c;
	struct pollfd p;
	CHECK(write(ready, "r", 1) == 1);
	if (operation == 0) {
		for (;;)
			getpid();
	}
	if (operation == 1) {
		p.fd = endpoint;
		p.events = POLLIN;
		poll(&p, 1, 10000);
	} else
		read(endpoint, &c, 1);
	return 0;
}
int main(void)
{
	int op, i, p[2], s[2];
	pid_t child;
	char c;
	pthread_t t;
	alarm(20);
	for (op = 0; op < 3; op++)
		for (i = 0; i < (op ? 8 : 32); i++) {
			CHECK(!pipe(p));
			if (op)
				CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
			child = fork();
			CHECK(child >= 0);
			if (!child) {
				close(p[0]);
				ready = p[1];
				operation = op;
				if (op) {
					close(s[0]);
					endpoint = s[1];
				}
				CHECK(!pthread_create(&t, 0, worker, 0));
				usleep(op ? 100000 : 20000);
				_exit(0);
			}
			close(p[1]);
			CHECK(read(p[0], &c, 1) == 1 && c == 'r');
			child_ok(child);
			close(p[0]);
			if (op) {
				CHECK(write(s[0], "x", 1) == 1);
				CHECK(read(s[1], &c, 1) == 1 && c == 'x');
				close(s[0]);
				close(s[1]);
			}
		}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
