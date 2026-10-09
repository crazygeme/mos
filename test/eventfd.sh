#!/bin/sh
# Validate event counters, descriptor flags, epoll notifications, and waits.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-eventfd.XXXXXX)
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

struct ep_event {
	uint32_t events;
	uint64_t data;
} __attribute__((packed));
static uint64_t value(int fd, int size)
{
	uint64_t b[2];
	CHECK(read(fd, b, size) == 8);
	return b[0];
}
static void put(int fd, uint64_t n)
{
	CHECK(write(fd, &n, 8) == 8);
}
int main(void)
{
	int fd, dup, ep, i;
	uint64_t max = UINT64_MAX - 1, n;
	char b[16];
	struct ep_event e, out;
	struct pollfd p;
	pid_t child;
	alarm(15);
	CHECK(syscall(328, 0, 0x100) == -1 && errno == EINVAL);
	CHECK((fd = syscall(328, 7, O_NONBLOCK | O_CLOEXEC)) >= 0);
	CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) &&
	      (fcntl(fd, F_GETFD) & FD_CLOEXEC));
	CHECK(value(fd, 16) == 7);
	CHECK(read(fd, b, 8) == -1 && errno == EAGAIN);
	CHECK(read(fd, b, 7) == -1 && errno == EINVAL);
	CHECK(write(fd, b, 7) == -1 && errno == EINVAL);
	CHECK(write(fd, b, 16) == -1 && errno == EINVAL);
	n = UINT64_MAX;
	CHECK(write(fd, &n, 8) == -1 && errno == EINVAL);
	CHECK(lseek(fd, 1, SEEK_SET) == 0);
	put(fd, 2);
	put(fd, 5);
	CHECK((dup = dup2(fd, 64)) == 64);
	CHECK(value(dup, 8) == 7);
	CHECK(read(fd, b, 8) == -1 && errno == EAGAIN);
	close(dup);
	CHECK((ep = syscall(254, 1)) >= 0);
	e.events = 1;
	e.data = fd;
	CHECK(!syscall(255, ep, 1, fd, &e));
	CHECK(!syscall(256, ep, &out, 1, 0));
	put(fd, 1);
	CHECK(syscall(256, ep, &out, 1, 500) == 1 && out.events == 1 &&
	      out.data == fd);
	CHECK(value(fd, 8) == 1);
	CHECK(!syscall(256, ep, &out, 1, 0));
	close(ep);
	put(fd, max);
	p.fd = fd;
	p.events = POLLOUT;
	CHECK(!poll(&p, 1, 0));
	n = 1;
	CHECK(write(fd, &n, 8) == -1 && errno == EAGAIN);
	CHECK(value(fd, 8) == max);
	CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT));
	close(fd);
	CHECK((fd = syscall(328, 3, 1 | O_NONBLOCK)) >= 0);
	for (i = 0; i < 3; i++)
		CHECK(value(fd, 8) == 1);
	CHECK(read(fd, b, 8) == -1 && errno == EAGAIN);
	close(fd);
	CHECK((fd = syscall(323, 0xffffffffU)) >= 0);
	CHECK(!(fcntl(fd, F_GETFL) & O_NONBLOCK) &&
	      !(fcntl(fd, F_GETFD) & FD_CLOEXEC));
	CHECK(value(fd, 8) == 0xffffffffU);
	close(fd);
	for (i = 0; i < 2; i++) {
		CHECK((fd = syscall(328, 0, 0)) >= 0);
		if (i)
			put(fd, max);
		child = fork();
		CHECK(child >= 0);
		if (!child) {
			usleep(50000);
			if (i)
				CHECK(value(fd, 8) == max);
			else
				put(fd, 9);
			close(fd);
			_exit(0);
		}
		if (i) {
			put(fd, 4);
			CHECK(value(fd, 8) == 4);
		} else
			CHECK(value(fd, 8) == 9);
		child_ok(child);
		close(fd);
	}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
