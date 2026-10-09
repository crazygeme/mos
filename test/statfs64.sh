#!/bin/sh
# Validate i386 filesystem statistics and cross-mount symbolic links.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-statfs64.XXXXXX)
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

struct fs64 {
	uint32_t type, bsize;
	uint64_t blocks, bfree, bavail, files, ffree;
	int32_t fsid[2];
	uint32_t namelen, frsize, flags, spare[4];
} __attribute__((packed));
int main(void)
{
	struct fs64 a, b;
	int fd;
	CHECK(sizeof(a) == 84 && (char *)&a.fsid - (char *)&a == 48 &&
	      (char *)&a.flags - (char *)&a == 64);
	CHECK(!syscall(268, "/", 84, &a));
	CHECK(a.bsize && a.blocks && a.bavail <= a.bfree &&
	      a.bfree <= a.blocks && (a.flags & 0x20));
	CHECK((fd = open("/", O_RDONLY)) >= 0);
	CHECK(!syscall(269, fd, 84, &b) && a.type == b.type &&
	      a.blocks == b.blocks && a.bsize == b.bsize);
	close(fd);
	CHECK(syscall(268, "/", 83, &a) == -1 && errno == EINVAL);
	CHECK(syscall(269, -1, 84, &a) == -1 && errno == EBADF);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
# RH9 has /bin/sh; the /usr/bin/sh alias is not part of its layout.
[ -L /bin/sh ]
[ -x /bin/sh ]
/bin/sh -c 'test -r /proc/cpuinfo'
grep processor /proc/cpuinfo
# RH9 maintains a regular mtab; exercise the procfs link with our own fixture.
ln -s /proc/mounts "$probe_dir/mtab"
cmp "$probe_dir/mtab" /proc/mounts
ln -s /proc/mounts "$probe_dir/absolute"
ln -s absolute "$probe_dir/relative"
ln -s "$probe_dir" "$probe_dir/directory"
cmp "$probe_dir/absolute" /proc/mounts
cmp "$probe_dir/relative" /proc/mounts
[ "$(readlink "$probe_dir/directory/absolute")" = /proc/mounts ]
cmp "$probe_dir/directory/relative" /proc/mounts
[ -L "$probe_dir/relative" ]
df -h
df -h /
