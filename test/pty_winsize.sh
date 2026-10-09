#!/bin/sh
# Validate PTY size updates and SIGWINCH delivery to the foreground group.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-pty_winsize.XXXXXX)
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

static int output;
static void winch(int sig)
{
	write(output, "W", 1);
}
static void event(int fd, char expected)
{
	struct pollfd p;
	char c;
	p.fd = fd;
	p.events = POLLIN;
	CHECK(poll(&p, 1, 3000) > 0 && read(fd, &c, 1) == 1 && c == expected);
}
int main(void)
{
	int master, slave, ready[2], stop[2], i, j, fd;
	pid_t pid;
	char c;
	struct winsize dims[] = { { 30, 120, 800, 600 },
				  { 40, 160, 1024, 768 },
				  { 40, 160, 1280, 960 } },
		       actual;
	struct pollfd p;
	alarm(10);
	CHECK(!openpty(&master, &slave, 0, 0, 0));
	CHECK(!pipe(ready) && !pipe(stop));
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		struct sigaction a;
		close(master);
		close(ready[0]);
		close(stop[1]);
		CHECK(setsid() >= 0 && !ioctl(slave, TIOCSCTTY, 0) &&
		      !tcsetpgrp(slave, getpgrp()));
		memset(&a, 0, sizeof(a));
		a.sa_handler = winch;
		a.sa_flags = SA_RESTART;
		output = ready[1];
		CHECK(!sigaction(SIGWINCH, &a, 0));
		CHECK(write(output, "R", 1) == 1);
		while (read(stop[0], &c, 1) == -1)
			CHECK(errno == EINTR);
		_exit(0);
	}
	close(ready[1]);
	close(stop[0]);
	event(ready[0], 'R');
	for (i = 0; i < 3; i++) {
		CHECK(!ioctl(i == 1 ? slave : master, TIOCSWINSZ, &dims[i]));
		event(ready[0], 'W');
		for (j = 0; j < 2; j++) {
			fd = j ? slave : master;
			CHECK(!ioctl(fd, TIOCGWINSZ, &actual) &&
			      !memcmp(&actual, &dims[i], sizeof(actual)));
			CHECK(!ioctl(fd, TIOCSWINSZ, &dims[i]));
			p.fd = ready[0];
			p.events = POLLIN;
			CHECK(!poll(&p, 1, 150));
		}
	}
	CHECK(write(stop[1], "Q", 1) == 1);
	close(stop[1]);
	child_ok(pid);
	close(ready[0]);
	close(slave);
	close(master);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe" -lutil
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
