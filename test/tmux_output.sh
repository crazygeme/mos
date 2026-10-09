#!/bin/sh
# Validate tmux output through a slow PTY without keyboard input.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
if ! command -v tmux >/dev/null 2>&1; then
    echo 'SKIP: tmux output check requires tmux in the guest'
    exit 0
fi
probe_dir=$(mktemp -d /tmp/mos-tmux_output.XXXXXX)
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

#include <pty.h>
#include <termios.h>
static void check(int rows, int cols)
{
	int master, slave, status, found = 0;
	pid_t child, pid;
	struct winsize size;
	char socket_path[128], b[512], output[131072];
	size_t used = 0;
	struct pollfd p;
	long ticks = 0;
	memset(&size, 0, sizeof(size));
	size.ws_row = rows;
	size.ws_col = cols;
	CHECK(!openpty(&master, &slave, 0, 0, &size));
	snprintf(socket_path, sizeof(socket_path), "/tmp/mos-tmux-%d-%d",
		 getpid(), rows);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(master);
		CHECK(setsid() >= 0 && !ioctl(slave, TIOCSCTTY, 0));
		CHECK(dup2(slave, 0) == 0 && dup2(slave, 1) == 1 &&
		      dup2(slave, 2) == 2);
		if (slave > 2)
			close(slave);
		unsetenv("TMUX");
		setenv("TERM", "xterm-256color", 1);
		execlp("tmux", "tmux", "-S", socket_path, "-f", "/dev/null",
		       "new-session",
		       "sleep 1; ls -alh /usr/lib; printf '\\nTMUX_OUTPUT_COMPLETE\\n'; sleep 30",
		       (char *)0);
		_exit(127);
	}
	close(slave);
	p.fd = master;
	p.events = POLLIN;
	while (ticks++ < 1500) {
		int n = poll(&p, 1, 100);
		CHECK(n >= 0);
		if (n) {
			ssize_t len = read(master, b, sizeof(b));
			if (len <= 0)
				break;
			CHECK(used + len < sizeof(output));
			memcpy(output + used, b, len);
			used += len;
			output[used] = 0;
			if (strstr(output, "TMUX_OUTPUT_COMPLETE")) {
				found = 1;
				break;
			}
			usleep(10000);
		}
	}
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		int null = open("/dev/null", O_WRONLY);
		dup2(null, 1);
		dup2(null, 2);
		execlp("tmux", "tmux", "-S", socket_path, "kill-server",
		       (char *)0);
		_exit(127);
	}
	waitpid(pid, &status, 0);
	close(master);
	kill(child, SIGKILL);
	waitpid(child, &status, 0);
	unlink(socket_path);
	CHECK(found);
}
int main(void)
{
	alarm(50);
	check(24, 80);
	check(36, 100);
	check(80, 240);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe" -lutil
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
