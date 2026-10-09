#!/bin/sh
# Validate sendfile offset handling and buffered copying.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-sendfile_copy.XXXXXX)
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

#include <sys/sendfile.h>
int main(void)
{
	int source = temp_file(), target = temp_file();
	size_t n = 1048576, i;
	unsigned char *p = malloc(n), *b = malloc(65537);
	off_t off = 3;
	CHECK(p && b);
	for (i = 0; i < n; i++)
		p[i] = i % 256;
	CHECK(write(source, p, n) == n);
	CHECK(lseek(source, 17, SEEK_SET) == 17);
	CHECK(sendfile(target, source, &off, 65537) == 65537 && off == 65540);
	CHECK(lseek(source, 0, SEEK_CUR) == 17 &&
	      lseek(target, 0, SEEK_CUR) == 65537);
	CHECK(pread(target, b, 65537, 0) == 65537 && !memcmp(b, p + 3, 65537));
	CHECK(lseek(target, 0, SEEK_SET) == 0);
	CHECK(sendfile(target, source, 0, 32769) == 32769);
	CHECK(lseek(source, 0, SEEK_CUR) == 32786);
	CHECK(pread(target, b, 32769, 0) == 32769 && !memcmp(b, p + 17, 32769));
	off = n;
	CHECK(sendfile(target, source, &off, 4096) == 0);
	off = 0;
	CHECK(sendfile(target, source, &off, 0) == 0);
	/* The legacy syscall writes a 32-bit offset without touching its neighbor. */
	{
		struct {
			int32_t offset;
			uint32_t canary;
		} legacy = { 3, 0xa5b6c7d8 };
		CHECK(lseek(target, 0, SEEK_SET) == 0);
		CHECK(syscall(187, target, source, &legacy.offset, 65537) ==
		      65537);
		CHECK(legacy.offset == 65540 && legacy.canary == 0xa5b6c7d8);
		CHECK(lseek(source, 0, SEEK_CUR) == 32786);
		CHECK(pread(target, b, 65537, 0) == 65537 &&
		      !memcmp(b, p + 3, 65537));
		legacy.offset = -1;
		CHECK(syscall(187, target, source, &legacy.offset, 1) == -1 &&
		      errno == EINVAL);
		CHECK(syscall(187, target, source, (void *)1, 1) == -1 &&
		      errno == EFAULT);
		CHECK(syscall(239, target, source, (void *)1, 1) == -1 &&
		      errno == EFAULT);
	}
	free(p);
	free(b);
	close(target);
	close(source);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
