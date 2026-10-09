#!/bin/sh
# Validate PTY write readiness and wakeups after draining or flushing.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-pty_poll_write.XXXXXX)
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

#include <termios.h>
#include <pty.h>
#include <sys/select.h>
static void raw(int fd)
{
	struct termios t;
	CHECK(!tcgetattr(fd, &t));
	cfmakeraw(&t);
	CHECK(!tcsetattr(fd, TCSANOW, &t));
}

static long milliseconds(void)
{
	struct timeval t;
	gettimeofday(&t, 0);
	return t.tv_sec * 1000 + t.tv_usec / 1000;
}
static void fill(int writer)
{
	char b[65536];
	int i;
	size_t total = 0;
	ssize_t n;
	struct pollfd p;
	fd_set f;
	struct timeval t;
	memset(b, 'x', sizeof(b));
	p.fd = writer;
	p.events = POLLOUT;
	for (i = 0; i < 512; i++) {
		n = write(writer, b, sizeof(b));
		if (n > 0) {
			total += n;
			CHECK(total < 4 * 1024 * 1024);
		} else {
			CHECK(n == -1 && errno == EAGAIN);
			FD_ZERO(&f);
			FD_SET(writer, &f);
			t.tv_sec = t.tv_usec = 0;
			if (!poll(&p, 1, 0) &&
			    !select(writer + 1, 0, &f, 0, &t))
				return;
			usleep(1000);
		}
	}
	CHECK(0);
}
int main(void)
{
	int direction, operation, api, master, slave, gate[2], writer, reader;
	pid_t child;
	char c, b[65536];
	struct pollfd p;
	fd_set f;
	struct timeval t;
	long start;
	alarm(30);
	for (direction = 0; direction < 2; direction++)
		for (operation = 0; operation < 2; operation++)
			for (api = 0; api < 3; api++) {
				CHECK(!openpty(&master, &slave, 0, 0, 0));
				raw(slave);
				CHECK(!fcntl(master, F_SETFL, O_NONBLOCK) &&
				      !fcntl(slave, F_SETFL, O_NONBLOCK));
				writer = direction ? master : slave;
				reader = direction ? slave : master;
				fill(writer);
				CHECK(!pipe(gate));
				child = fork();
				CHECK(child >= 0);
				if (!child) {
					close(gate[1]);
					CHECK(read(gate[0], &c, 1) == 1);
					usleep(100000);
					if (operation)
						CHECK(!tcflush(slave,
							       TCIOFLUSH));
					else {
						ssize_t n;
						while ((n = read(reader, b,
								 sizeof(b))) >
						       0) {
						}
						CHECK(n == 0 ||
						      (n == -1 &&
						       errno == EAGAIN));
					}
					CHECK(read(gate[0], &c, 1) == 1);
					_exit(0);
				}
				close(gate[0]);
				start = milliseconds();
				CHECK(write(gate[1], "R", 1) == 1);
				if (api == 2) {
					FD_ZERO(&f);
					FD_SET(writer, &f);
					t.tv_sec = 2;
					t.tv_usec = 0;
					CHECK(select(writer + 1, 0, &f, 0,
						     &t) == 1 &&
					      FD_ISSET(writer, &f));
				} else {
					p.fd = writer;
					p.events = POLLOUT | (api ? POLLIN : 0);
					CHECK(poll(&p, 1, 2000) == 1 &&
					      (p.revents & POLLOUT));
				}
				if (!operation)
					CHECK(milliseconds() - start < 1000);
				CHECK(write(writer, "Y", 1) == 1);
				CHECK(write(gate[1], "D", 1) == 1);
				child_ok(child);
				close(gate[1]);
				close(slave);
				close(master);
			}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe" -lutil
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
