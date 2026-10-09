#!/bin/sh
# Validate route netlink snapshots and libc interface enumeration.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-netlink_interfaces.XXXXXX)
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

#include <ifaddrs.h>
struct nl_header {
	uint32_t len;
	uint16_t type, flags;
	uint32_t seq, pid;
};
struct nl_addr {
	unsigned short family, pad;
	uint32_t pid, groups;
};
int main(void)
{
	int message, fd, n, peek, offset, count, done;
	unsigned char b[65536], copy[65536];
	struct {
		struct nl_header h;
		unsigned char family;
	} __attribute__((packed)) req;
	struct nl_addr addr;
	socklen_t size;
	struct nl_header *h;
	struct ifaddrs *head, *cur;
	int lo = 0, others = 0;
	alarm(10);
	for (message = 18; message <= 22; message += 4) {
		CHECK((fd = socket(AF_NETLINK, SOCK_RAW, 0)) >= 0);
		memset(&addr, 0, sizeof(addr));
		addr.family = AF_NETLINK;
		CHECK(!bind(fd, (void *)&addr, sizeof(addr)));
		size = sizeof(addr);
		CHECK(!getsockname(fd, (void *)&addr, &size) && addr.pid);
		memset(&req, 0, sizeof(req));
		req.h.len = 17;
		req.h.type = message;
		req.h.flags = 0x301;
		req.h.seq = message + 100;
		addr.pid = 0;
		CHECK(sendto(fd, &req, 17, 0, (void *)&addr, sizeof(addr)) ==
		      17);
		CHECK((peek = recv(fd, copy, sizeof(copy), MSG_PEEK)) > 0);
		CHECK((n = recv(fd, b, sizeof(b), 0)) == peek &&
		      !memcmp(b, copy, n));
		size = sizeof(addr);
		CHECK(!getsockname(fd, (void *)&addr, &size));
		count = done = 0;
		while (!done) {
			for (offset = 0; offset < n;
			     offset += (h->len + 3) & ~3) {
				CHECK(offset + 16 <= n);
				h = (void *)(b + offset);
				CHECK(h->len >= 16 && offset + h->len <= n &&
				      h->seq == message + 100 &&
				      h->pid == addr.pid);
				if (h->type == 3) {
					CHECK(h->len >= 20 &&
					      *(int *)(b + offset + 16) == 0);
					done = 1;
					break;
				}
				CHECK(h->type == message - 2 && (h->flags & 2));
				count++;
			}
			if (!done)
				CHECK((n = recv(fd, b, sizeof(b), 0)) > 0);
		}
		CHECK(count);
		close(fd);
	}
	CHECK(!getifaddrs(&head));
	for (cur = head; cur; cur = cur->ifa_next) {
		if (!strcmp(cur->ifa_name, "lo"))
			lo = 1;
		else
			others = 1;
	}
	CHECK(lo && others);
	freeifaddrs(head);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
