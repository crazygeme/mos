#!/bin/sh
# Validate stream descriptor queue backpressure and partial sends.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_rights_backpressure.XXXXXX)
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

static int temp_file(void)
{
	char path[] = "/tmp/mos-probe.XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	CHECK(!unlink(path));
	return fd;
}

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

static int s[2], backing;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int completed;
static void *sender(void *unused)
{
	CHECK(send_fd(s[0], backing, "C", 1) == 1);
	pthread_mutex_lock(&lock);
	completed = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}
static void receive(char value)
{
	int fd;
	char c;
	struct stat st;
	CHECK(receive_fd(s[1], &c, 1, &fd, 0) == 1 && c == value && fd >= 0);
	CHECK(!fstat(fd, &st) && st.st_size == 4096);
	close(fd);
}
int main(void)
{
	int count = 0, i, fd;
	ssize_t r, sent;
	char *payload, b[32768];
	pthread_t t;
	struct pollfd p;
	alarm(10);
	backing = temp_file();
	CHECK(!ftruncate(backing, 4096));
	CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
	CHECK(!fcntl(s[0], F_SETFL, O_NONBLOCK));
	while (count < 10000) {
		r = send_fd(s[0], backing, "A", 1);
		if (r == -1) {
			CHECK(errno == EAGAIN);
			break;
		}
		CHECK(r == 1);
		count++;
	}
	CHECK(count > 1 && count < 10000);
	p.fd = s[0];
	p.events = POLLOUT;
	CHECK(!poll(&p, 1, 0));
	receive('A');
	CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLOUT));
	CHECK(send_fd(s[0], backing, "B", 1) == 1);
	CHECK(!fcntl(s[0], F_SETFL, 0));
	CHECK(!pthread_create(&t, 0, sender, 0));
	usleep(50000);
	pthread_mutex_lock(&lock);
	CHECK(!completed);
	pthread_mutex_unlock(&lock);
	receive('A');
	CHECK(!pthread_join(t, 0));
	CHECK(completed);
	for (i = 0; i < count - 2; i++)
		receive('A');
	receive('B');
	receive('C');
	CHECK(!fcntl(s[0], F_SETFL, O_NONBLOCK));
	payload = malloc(4 * 1024 * 1024);
	CHECK(payload);
	memset(payload, 'P', 4 * 1024 * 1024);
	sent = send_fd(s[0], backing, payload, 4 * 1024 * 1024);
	CHECK(sent > 0 && sent < 4 * 1024 * 1024);
	receive('P');
	sent--;
	while (sent) {
		r = receive_fd(s[1], b, sent > sizeof(b) ? sizeof(b) : sent,
			       &fd, 0);
		CHECK(r > 0 && fd == -1);
		for (i = 0; i < r; i++)
			CHECK(b[i] == 'P');
		sent -= r;
	}
	free(payload);
	close(backing);
	close(s[0]);
	close(s[1]);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
