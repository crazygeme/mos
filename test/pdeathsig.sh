#!/bin/sh
# Validate parent-death signals inside a MOS guest using the guest C compiler.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-pdeathsig.XXXXXX)
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

#include <sys/prctl.h>
static int output;
static void received(int sig)
{
	write(output, "D", 1);
	_exit(0);
}
static int get(void)
{
	int n = -1;
	CHECK(!prctl(2, &n, 0, 0, 0));
	return n;
}
static void set(int n)
{
	CHECK(!prctl(1, n, 0, 0, 0));
}
int main(int argc, char **argv)
{
	int p[2], ready[2];
	pid_t pid, parent;
	char c;
	struct pollfd poller;
	alarm(15);
	if (argc > 1) {
		CHECK(get() == SIGUSR1);
		return 0;
	}
	set(SIGUSR1);
	CHECK(get() == SIGUSR1);
	CHECK(prctl(1, -1, 0, 0, 0) == -1 && errno == EINVAL);
	CHECK(get() == SIGUSR1);
	CHECK(prctl(2, 0, 0, 0, 0) == -1 && errno == EFAULT);
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		CHECK(!get());
		set(SIGUSR1);
		execl(argv[0], argv[0], "--exec", (char *)0);
		_exit(1);
	}
	child_ok(pid);
	CHECK(get() == SIGUSR1);
	set(0);
	CHECK(!get());
	CHECK(!pipe(p));
	parent = fork();
	CHECK(parent >= 0);
	if (!parent) {
		close(p[0]);
		CHECK(!pipe(ready));
		pid = fork();
		CHECK(pid >= 0);
		if (!pid) {
			close(ready[0]);
			output = p[1];
			signal(SIGUSR1, received);
			alarm(5);
			set(SIGUSR1);
			CHECK(write(ready[1], "R", 1) == 1);
			close(ready[1]);
			for (;;)
				pause();
		}
		close(ready[1]);
		CHECK(read(ready[0], &c, 1) == 1 && c == 'R');
		_exit(0);
	}
	close(p[1]);
	child_ok(parent);
	poller.fd = p[0];
	poller.events = POLLIN;
	CHECK(poll(&poller, 1, 7000) > 0);
	CHECK(read(p[0], &c, 1) == 1 && c == 'D');
	close(p[0]);
	if (!geteuid()) {
		pid = fork();
		CHECK(pid >= 0);
		if (!pid) {
			set(SIGUSR1);
			CHECK(!setgid(65534) && !get());
			set(SIGUSR1);
			CHECK(!setuid(65534) && !get());
			_exit(0);
		}
		child_ok(pid);
	}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
