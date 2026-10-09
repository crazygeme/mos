#!/bin/sh
# Exercise concurrent descriptor reception, socket polling, and socket ioctls.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-fd_callback_threads.XXXXXX)
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

static ssize_t send_fd(int s, int fd, const void *data, size_t n)
{
	struct msghdr m;
	struct iovec v;
	union {
		struct cmsghdr align;
		char b[CMSG_SPACE(sizeof(int))];
	} c;
	struct cmsghdr *h;
	memset(&m, 0, sizeof(m));
	memset(&c, 0, sizeof(c));
	v.iov_base = (void *)data;
	v.iov_len = n;
	m.msg_iov = &v;
	m.msg_iovlen = 1;
	m.msg_control = c.b;
	m.msg_controllen = sizeof(c.b);
	h = CMSG_FIRSTHDR(&m);
	h->cmsg_level = SOL_SOCKET;
	h->cmsg_type = SCM_RIGHTS;
	h->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(h), &fd, sizeof(fd));
	return sendmsg(s, &m, 0);
}
static ssize_t receive_fd(int s, void *data, size_t n, int *fd, int flags)
{
	struct msghdr m;
	struct iovec v;
	union {
		struct cmsghdr align;
		char b[128];
	} c;
	struct cmsghdr *h;
	ssize_t r;
	memset(&m, 0, sizeof(m));
	memset(&c, 0, sizeof(c));
	v.iov_base = data;
	v.iov_len = n;
	m.msg_iov = &v;
	m.msg_iovlen = 1;
	m.msg_control = c.b;
	m.msg_controllen = sizeof(c.b);
	r = recvmsg(s, &m, flags);
	CHECK(r >= 0 && !(m.msg_flags & MSG_CTRUNC));
	h = CMSG_FIRSTHDR(&m);
	*fd = -1;
	if (h) {
		CHECK(h->cmsg_level == SOL_SOCKET &&
		      h->cmsg_type == SCM_RIGHTS &&
		      h->cmsg_len == CMSG_LEN(sizeof(int)));
		memcpy(fd, CMSG_DATA(h), sizeof(*fd));
		CHECK(!CMSG_NXTHDR(&m, h));
	}
	return r;
}

static int s[2], passed;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int stop;
static void *watch(void *unused)
{
	struct pollfd p;
	int n, done;
	p.fd = s[1];
	p.events = POLLIN | POLLOUT;
	for (;;) {
		pthread_mutex_lock(&lock);
		done = stop;
		pthread_mutex_unlock(&lock);
		if (done)
			break;
		CHECK(poll(&p, 1, 0) >= 0);
		CHECK(!ioctl(s[1], FIONREAD, &n));
		usleep(100);
	}
	return 0;
}
static void *send_loop(void *unused)
{
	int i;
	char c;
	for (i = 0; i < 5000; i++) {
		CHECK(send_fd(s[0], passed, "x", 1) == 1);
		CHECK(read(s[0], &c, 1) == 1 && c == 'a');
	}
	return 0;
}
static void *waiter(void *arg)
{
	struct pollfd p;
	p.fd = (int)(intptr_t)arg;
	p.events = POLLIN;
	CHECK(poll(&p, 1, 200) >= 0);
	return 0;
}
int main(void)
{
	pthread_t watcher, writer, t;
	int i, fd;
	char c;
	alarm(30);
	signal(SIGPIPE, SIG_IGN);
	CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
	CHECK((passed = open("/dev/null", O_RDONLY)) >= 0);
	CHECK(!pthread_create(&watcher, 0, watch, 0));
	CHECK(!pthread_create(&writer, 0, send_loop, 0));
	for (i = 0; i < 5000; i++) {
		CHECK(receive_fd(s[1], &c, 1, &fd, 0) == 1 && c == 'x' &&
		      fd >= 0);
		close(fd);
		CHECK(write(s[1], "a", 1) == 1);
	}
	pthread_mutex_lock(&lock);
	stop = 1;
	pthread_mutex_unlock(&lock);
	CHECK(!pthread_join(watcher, 0));
	CHECK(!pthread_join(writer, 0));
	close(s[0]);
	close(s[1]);
	close(passed);
	for (i = 0; i < 64; i++) {
		CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
		CHECK(!pthread_create(&t, 0, waiter, (void *)(intptr_t)s[1]));
		usleep(10000);
		close(s[1]);
		if (write(s[0], "x", 1) == -1)
			CHECK(errno == EPIPE);
		CHECK(!pthread_join(t, 0));
		close(s[0]);
	}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
