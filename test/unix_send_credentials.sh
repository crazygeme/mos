#!/bin/sh
# Validate explicit Unix credentials with SO_PASSCRED disabled.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_send_credentials.XXXXXX)
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

int main(void)
{
	int s[2];
	struct ucred cred;
	struct msghdr m, r;
	struct iovec v, w;
	union {
		struct cmsghdr align;
		char b[128];
	} c, d;
	struct cmsghdr *h;
	char out = 0, in = 1;
	CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, s));
	memset(&m, 0, sizeof(m));
	memset(&c, 0, sizeof(c));
	v.iov_base = &out;
	v.iov_len = 1;
	m.msg_iov = &v;
	m.msg_iovlen = 1;
	m.msg_control = c.b;
	m.msg_controllen = CMSG_SPACE(sizeof(cred));
	h = CMSG_FIRSTHDR(&m);
	h->cmsg_level = SOL_SOCKET;
	h->cmsg_type = SCM_CREDENTIALS;
	h->cmsg_len = CMSG_LEN(sizeof(cred));
	cred.pid = getpid();
	cred.uid = getuid();
	cred.gid = getgid();
	memcpy(CMSG_DATA(h), &cred, sizeof(cred));
	CHECK(sendmsg(s[0], &m, 0) == 1);
	memset(&r, 0, sizeof(r));
	w.iov_base = &in;
	w.iov_len = 1;
	r.msg_iov = &w;
	r.msg_iovlen = 1;
	r.msg_control = d.b;
	r.msg_controllen = sizeof(d);
	CHECK(recvmsg(s[1], &r, 0) == 1 && in == 0 && !CMSG_FIRSTHDR(&r));
	h->cmsg_len = CMSG_LEN(4);
	m.msg_controllen = CMSG_SPACE(4);
	CHECK(sendmsg(s[0], &m, 0) == -1 && errno == EINVAL);
	close(s[0]);
	close(s[1]);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
