#!/bin/sh
# Validate Unix sequenced packets, ancillary data, readiness, and shutdown.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_seqpacket.XXXXXX)
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

static ssize_t record(int fd, void *b, size_t n, int flags, int *outflags,
		      struct ucred *cred, int *rights, int *count)
{
	struct msghdr m;
	struct iovec v;
	union {
		struct cmsghdr align;
		char b[256];
	} c;
	struct cmsghdr *h;
	ssize_t r;
	memset(&m, 0, sizeof(m));
	v.iov_base = b;
	v.iov_len = n;
	m.msg_iov = &v;
	m.msg_iovlen = 1;
	m.msg_control = c.b;
	m.msg_controllen = sizeof(c);
	r = recvmsg(fd, &m, flags);
	*outflags = m.msg_flags;
	*count = 0;
	if (cred)
		cred->pid = 0;
	for (h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
		CHECK(h->cmsg_level == SOL_SOCKET);
		if (h->cmsg_type == SCM_CREDENTIALS) {
			CHECK(cred && h->cmsg_len == CMSG_LEN(sizeof(*cred)));
			memcpy(cred, CMSG_DATA(h), sizeof(*cred));
		} else {
			CHECK(h->cmsg_type == SCM_RIGHTS);
			*count = (h->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			CHECK(*count <= 16);
			memcpy(rights, CMSG_DATA(h), *count * sizeof(int));
		}
	}
	return r;
}
int main(void)
{
	int s[2], fd, n, i, j, flags, count, passed[16], first, second,
		one = 1, zero = 0, records, listener, client, accepted;
	char b[80000], *large;
	struct ucred cred;
	struct pollfd p;
	pid_t child;
	struct sockaddr_un addr;
	alarm(20);
	CHECK((fd = socket(AF_UNIX, SOCK_SEQPACKET, 0)) >= 0);
	CHECK(recv(fd, b, 1, MSG_DONTWAIT) == -1 && errno == ENOTCONN);
	close(fd);
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, s));
	{
		socklen_t len = sizeof(n);
		CHECK(!getsockopt(s[0], SOL_SOCKET, SO_TYPE, &n, &len) &&
		      n == SOCK_SEQPACKET);
	}
	CHECK(send(s[0], "first-record", 12, 0) == 12 &&
	      send(s[0], "second", 6, 0) == 6);
	CHECK(!ioctl(s[1], FIONREAD, &n) && n == 18);
	CHECK(record(s[1], b, 5, MSG_PEEK, &flags, 0, passed, &count) == 5 &&
	      !memcmp(b, "first", 5) && (flags & MSG_TRUNC));
	CHECK(!ioctl(s[1], FIONREAD, &n) && n == 18);
	CHECK(record(s[1], b, 5, 0, &flags, 0, passed, &count) == 5 &&
	      !memcmp(b, "first", 5) && (flags & MSG_TRUNC));
	CHECK(recv(s[1], b, 64, 0) == 6 && !memcmp(b, "second", 6));
	CHECK(send(s[0], "", 0, 0) == 0 &&
	      send(s[0], "after-empty", 11, 0) == 11);
	p.fd = s[1];
	p.events = POLLIN;
	CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
	CHECK(recv(s[1], b, 1, 0) == 0);
	CHECK(recv(s[1], b, 64, 0) == 11 && !memcmp(b, "after-empty", 11));
	CHECK(send(s[0], "large", 5, 0) == 5);
	CHECK(recv(s[1], b, 2, MSG_TRUNC) == 5 && !memcmp(b, "la", 2));
	CHECK(recv(s[1], b, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
	for (i = 0; i < 75300; i++)
		b[i] = i % 251;
	CHECK(write(s[0], b, 75300) == 75300);
	memset(b, 0, 75300);
	CHECK(read(s[1], b, 75300) == 75300);
	for (i = 0; i < 75300; i++)
		CHECK((unsigned char)b[i] == i % 251);
	large = malloc(512 * 1024);
	CHECK(large);
	CHECK(send(s[0], large, 512 * 1024, 0) == -1 && errno == EMSGSIZE);
	free(large);
	CHECK(recv(s[1], b, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
	CHECK(!setsockopt(s[1], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)));
	first = temp_file();
	second = temp_file();
	CHECK(write(first, "one", 3) == 3 && write(second, "two", 3) == 3);
	CHECK(lseek(first, 0, SEEK_SET) == 0 &&
	      lseek(second, 0, SEEK_SET) == 0);
	CHECK(send_fd(s[0], first, "one", 3) == 3 &&
	      send_fd(s[0], second, "two", 3) == 3);
	for (i = 0; i < 3; i++) {
		CHECK(record(s[1], b, 64, i == 0 ? MSG_PEEK : 0, &flags, &cred,
			     passed, &count) == 3 &&
		      !memcmp(b, i < 2 ? "one" : "two", 3) && !flags);
		CHECK(cred.pid == getpid() && cred.uid == getuid() &&
		      cred.gid == getgid() && count == 1);
		CHECK(lseek(passed[0], 0, SEEK_SET) == 0);
		CHECK(read(passed[0], b, 3) == 3 &&
		      !memcmp(b, i < 2 ? "one" : "two", 3));
		close(passed[0]);
	}
	close(first);
	close(second);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(s[1]);
		CHECK(send(s[0], "child", 5, 0) == 5);
		_exit(0);
	}
	CHECK(record(s[1], b, 64, 0, &flags, &cred, passed, &count) == 5 &&
	      !memcmp(b, "child", 5) && !flags && cred.pid == child &&
	      cred.uid == getuid() && cred.gid == getgid());
	child_ok(child);
	CHECK(!setsockopt(s[1], SOL_SOCKET, SO_PASSCRED, &zero, sizeof(zero)));
	{
		struct msghdr m;
		struct iovec v;
		union {
			struct cmsghdr align;
			char b[CMSG_SPACE(16 * sizeof(int))];
		} c;
		struct cmsghdr *h;
		fd = temp_file();
		for (i = 0; i < 16; i++)
			passed[i] = fd;
		memset(&m, 0, sizeof(m));
		v.iov_base = "sixteen";
		v.iov_len = 7;
		m.msg_iov = &v;
		m.msg_iovlen = 1;
		m.msg_control = c.b;
		m.msg_controllen = sizeof(c);
		h = CMSG_FIRSTHDR(&m);
		h->cmsg_level = SOL_SOCKET;
		h->cmsg_type = SCM_RIGHTS;
		h->cmsg_len = CMSG_LEN(sizeof(passed));
		memcpy(CMSG_DATA(h), passed, sizeof(passed));
		CHECK(sendmsg(s[0], &m, 0) == 7);
		CHECK(record(s[1], b, 64, 0, &flags, 0, passed, &count) == 7 &&
		      !memcmp(b, "sixteen", 7) && !flags && count == 16);
		for (i = 0; i < 16; i++)
			close(passed[i]);
		close(fd);
	}
	CHECK(!fcntl(s[0], F_SETFL, O_NONBLOCK));
	memset(b, 'q', 1024);
	records = 0;
	while ((n = send(s[0], b, 1024, 0)) >= 0) {
		CHECK(n == 1024 && ++records < 10000);
	}
	CHECK(errno == EAGAIN);
	for (i = 0; i < records; i++) {
		CHECK(recv(s[1], b, 2048, 0) == 1024);
		for (j = 0; j < 1024; j++)
			CHECK(b[j] == 'q');
	}
	CHECK(recv(s[1], b, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
	CHECK(send(s[0], "after-full", 10, 0) == 10 &&
	      recv(s[1], b, 64, 0) == 10 && !memcmp(b, "after-full", 10));
	CHECK(!shutdown(s[0], SHUT_WR));
	CHECK(recv(s[1], b, 1, 0) == 0);
	CHECK(send(s[1], "reply", 5, 0) == 5 && recv(s[0], b, 64, 0) == 5 &&
	      !memcmp(b, "reply", 5));
	close(s[1]);
	CHECK(recv(s[0], b, 1, 0) == 0);
	CHECK(send(s[0], "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
	close(s[0]);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, "seqpacket.socket");
	CHECK((listener = socket(AF_UNIX, SOCK_SEQPACKET, 0)) >= 0);
	CHECK(!bind(listener, (void *)&addr, sizeof(addr)) &&
	      !listen(listener, 1));
	CHECK((client = socket(AF_UNIX, SOCK_SEQPACKET, 0)) >= 0);
	CHECK(!connect(client, (void *)&addr, sizeof(addr)));
	CHECK((accepted = accept(listener, 0, 0)) >= 0);
	CHECK(send(client, "first", 5, 0) == 5 &&
	      send(client, "second", 6, 0) == 6);
	CHECK(recv(accepted, b, 64, 0) == 5 && !memcmp(b, "first", 5));
	CHECK(recv(accepted, b, 64, 0) == 6 && !memcmp(b, "second", 6));
	close(client);
	close(accepted);
	close(listener);
	unlink(addr.sun_path);
	CHECK(!socketpair(AF_UNIX,
			  SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, s));
	for (i = 0; i < 2; i++) {
		CHECK((fcntl(s[i], F_GETFL) & O_NONBLOCK) &&
		      (fcntl(s[i], F_GETFD) & FD_CLOEXEC));
		close(s[i]);
	}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
