#!/bin/sh
# Validate IPC bulk transfers, ring wrapping, readiness, and shutdown.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-ipc_buffers.XXXXXX)
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

static void endpoints(int kind, int *reader, int *writer)
{
	int s[2], listener;
	struct sockaddr_un addr;
	if (kind == 0) {
		CHECK(!pipe(s));
		*reader = s[0];
		*writer = s[1];
	} else if (kind == 1) {
		unlink("fifo");
		CHECK(!mkfifo("fifo", 0600));
		CHECK((*reader = open("fifo", O_RDONLY | O_NONBLOCK)) >= 0);
		CHECK((*writer = open("fifo", O_WRONLY)) >= 0);
		CHECK(!fcntl(*reader, F_SETFL, 0));
		unlink("fifo");
	} else if (kind == 2) {
		CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
		*reader = s[0];
		*writer = s[1];
	} else {
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, "socket");
		unlink("socket");
		CHECK((listener = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
		CHECK(!bind(listener, (void *)&addr, sizeof(addr)) &&
		      !listen(listener, 1));
		CHECK((*writer = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
		CHECK(!connect(*writer, (void *)&addr, sizeof(addr)));
		CHECK((*reader = accept(listener, 0, 0)) >= 0);
		close(listener);
		unlink("socket");
	}
}
static void transfer(int kind, size_t fragment)
{
	int reader, writer;
	size_t size = 251 * (fragment == 1 ? 17 : 8357), offset, n, i;
	unsigned char *payload = malloc(size), b[65521];
	ssize_t r;
	pid_t child;
	CHECK(payload);
	for (i = 0; i < size; i++)
		payload[i] = i % 251;
	endpoints(kind, &reader, &writer);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(reader);
		for (offset = 0; offset < size; offset += r) {
			n = size - offset;
			if (n > fragment)
				n = fragment;
			r = write(writer, payload + offset, n);
			CHECK(r > 0);
		}
		close(writer);
		_exit(0);
	}
	close(writer);
	offset = 0;
	while ((r = read(reader, b, sizeof(b))) > 0) {
		CHECK(offset + r <= size && !memcmp(b, payload + offset, r));
		offset += r;
	}
	CHECK(r == 0 && offset == size);
	child_ok(child);
	close(reader);
	free(payload);
}
static void readiness(int kind)
{
	int reader, writer;
	unsigned char b[65536];
	size_t i, filled = 0, offset = 0, n;
	ssize_t r;
	struct pollfd p;
	endpoints(kind, &reader, &writer);
	CHECK(!fcntl(reader, F_SETFL, O_NONBLOCK) &&
	      !fcntl(writer, F_SETFL, O_NONBLOCK));
	CHECK(read(reader, b, 1) == -1 && errno == EAGAIN);
	for (i = 0; i < sizeof(b); i++)
		b[i] = i % 256;
	while ((r = write(writer, b, sizeof(b))) > 0)
		filled += r;
	CHECK(r == -1 && errno == EAGAIN && filled);
	p.fd = writer;
	p.events = POLLOUT;
	CHECK(!poll(&p, 1, 0));
	while (offset < filled) {
		n = filled - offset;
		if (n > 4093)
			n = 4093;
		r = read(reader, b, n);
		CHECK(r > 0);
		for (i = 0; i < r; i++)
			CHECK(b[i] == (offset + i) % 256);
		offset += r;
	}
	CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT));
	close(reader);
	CHECK(write(writer, "x", 1) == -1 && errno == EPIPE);
	close(writer);
}
static void datagrams(void)
{
	int s[2], i;
	char b[3001], *large = malloc(1048576);
	CHECK(large && !socketpair(AF_UNIX, SOCK_DGRAM, 0, s));
	memset(b, 'a', sizeof(b));
	for (i = 0; i < 10; i++) {
		CHECK(send(s[0], b, sizeof(b), 0) == sizeof(b));
		CHECK(recv(s[1], b, 7, 0) == 7 && !memcmp(b, "aaaaaaa", 7));
		CHECK(send(s[0], "next", 4, 0) == 4 &&
		      recv(s[1], b, 100, 0) == 4 && !memcmp(b, "next", 4));
		memset(b, 'a', sizeof(b));
	}
	CHECK(send(s[0], large, 1048576, 0) == -1 &&
	      (errno == ENOBUFS || errno == EMSGSIZE));
	free(large);
	close(s[0]);
	close(s[1]);
}
static void timeouts(void)
{
	int s[2];
	char b[65536];
	struct timeval t = { 0, 20000 };
	ssize_t r;
	CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
	CHECK(!setsockopt(s[1], SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)));
	CHECK(read(s[1], b, 1) == -1 && errno == EAGAIN);
	CHECK(!fcntl(s[0], F_SETFL, O_NONBLOCK));
	memset(b, 'x', sizeof(b));
	while (write(s[0], b, sizeof(b)) > 0) {
	}
	CHECK(errno == EAGAIN);
	CHECK(!fcntl(s[0], F_SETFL, 0));
	CHECK(!setsockopt(s[0], SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t)));
	CHECK(write(s[0], b, 1) == -1 && errno == EAGAIN);
	t.tv_usec = 0;
	CHECK(!setsockopt(s[1], SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)));
	CHECK(!shutdown(s[0], SHUT_WR));
	while ((r = recv(s[1], b, sizeof(b), 0)) > 0) {
	}
	CHECK(r == 0);
	close(s[0]);
	close(s[1]);
}
static void descriptors(void)
{
	int s[2], p[2], i, fd, count, held = -1;
	size_t size = 251 * 399, j, offset;
	unsigned char *payload = malloc(size), *b = malloc(size);
	ssize_t n;
	CHECK(payload && b);
	for (j = 0; j < size; j++)
		payload[j] = j % 251;
	CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s) && !pipe(p));
	for (i = 0; i < 6; i++) {
		CHECK(send_fd(s[0], p[0], payload, size) == size);
		offset = count = 0;
		while (offset < size) {
			n = receive_fd(s[1], b + offset, size - offset, &fd, 0);
			CHECK(n > 0);
			offset += n;
			if (fd >= 0) {
				count++;
				held = fd;
			}
		}
		CHECK(count == 1 && !memcmp(b, payload, size));
		CHECK(write(p[1], "fd", 2) == 2 && read(held, b, 2) == 2 &&
		      !memcmp(b, "fd", 2));
		close(held);
	}
	free(payload);
	free(b);
	close(s[0]);
	close(s[1]);
	close(p[0]);
	close(p[1]);
}
int main(void)
{
	size_t fragments[] = { 1, 4093, 65536, 262147 };
	int kind, i;
	alarm(60);
	signal(SIGPIPE, SIG_IGN);
	for (kind = 0; kind < 4; kind++)
		for (i = 0; i < 4; i++)
			transfer(kind, fragments[i]);
	readiness(0);
	readiness(2);
	datagrams();
	timeouts();
	descriptors();
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
