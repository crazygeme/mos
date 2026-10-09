#!/bin/sh
# Validate descriptor delivery with the first partial stream read.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_rights_partial.XXXXXX)
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

int main(void)
{
	char path[] = "/dev/shm/mos-rights.XXXXXX", b[128];
	int s[2], fd = mkstemp(path), received;
	size_t page = getpagesize(), i;
	unsigned char *original, *shared;
	CHECK(fd >= 0);
	unlink(path);
	CHECK(!ftruncate(fd, page));
	original = mmap(0, page, 3, MAP_SHARED, fd, 0);
	CHECK(original != MAP_FAILED);
	memset(original, 'P', page);
	CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
	CHECK(send_fd(s[0], fd, "headerpayload", 13) == 13);
	CHECK(receive_fd(s[1], b, 3, &received, MSG_CMSG_CLOEXEC) == 3 &&
	      !memcmp(b, "hea", 3) && received >= 0);
	CHECK(fcntl(received, F_GETFD) & FD_CLOEXEC);
	shared = mmap(0, page, 3, MAP_SHARED, received, 0);
	CHECK(shared != MAP_FAILED);
	for (i = 0; i < page; i++)
		CHECK(shared[i] == 'P');
	shared[0] = 7;
	CHECK(original[0] == 7);
	munmap(shared, page);
	close(received);
	CHECK(receive_fd(s[1], b, 128, &received, 0) == 10 &&
	      !memcmp(b, "derpayload", 10) && received == -1);
	munmap(original, page);
	close(fd);
	close(s[0]);
	close(s[1]);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
