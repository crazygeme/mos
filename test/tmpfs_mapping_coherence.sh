#!/bin/sh
# Validate tmpfs mapping coherence across growth, file I/O, and page discard.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-tmpfs_mapping_coherence.XXXXXX)
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
	char path[] = "/dev/shm/mos-coherence.XXXXXX";
	int fd = mkstemp(path), ro;
	size_t page = getpagesize(), i;
	unsigned char *first, *second, *readonly, *private, *grown, b[8];
	CHECK(fd >= 0);
	CHECK(!ftruncate(fd, page));
	first = mmap(0, page, 3, MAP_SHARED, fd, 0);
	CHECK(first != MAP_FAILED);
	memset(first, 'P', page);
	CHECK(!ftruncate(fd, 2 * page));
	CHECK((ro = open(path, O_RDONLY)) >= 0);
	second = mmap(0, 2 * page, 3, MAP_SHARED, fd, 0);
	readonly = mmap(0, 2 * page, 1, MAP_SHARED, ro, 0);
	private = mmap(0, page, 3, MAP_PRIVATE, fd, 0);
	CHECK(second != MAP_FAILED && readonly != MAP_FAILED &&
	      private != MAP_FAILED);
	for (i = 0; i < 2 * page; i++)
		CHECK(second[i] == (i < page ? 'P' : 0) &&
		      readonly[i] == second[i]);
	CHECK(pread(fd, b, 8, 0) == 8 && !memcmp(b, "PPPPPPPP", 8));
	CHECK(pwrite(fd, "Z", 1, 11) == 1);
	CHECK(first[11] == 'Z' && second[11] == 'Z' && readonly[11] == 'Z');
	private[12] = 7;
	CHECK(first[12] == 'P' && second[12] == 'P');
	first[13] = 'A';
	CHECK(private[13] == 'P');
	CHECK(!madvise(private, page, MADV_DONTNEED));
	CHECK(private[12] == 'P' && private[13] == 'A');
	CHECK(!madvise(second, 2 * page, MADV_DONTNEED));
	CHECK(second[11] == 'Z' && second[13] == 'A');
	CHECK(!ftruncate(fd, 3 * page));
	grown = mmap(0, 3 * page, 3, MAP_SHARED, fd, 0);
	CHECK(grown != MAP_FAILED && !memcmp(grown, first, page));
	for (i = 2 * page; i < 3 * page; i++)
		CHECK(!grown[i]);
	munmap(grown, 3 * page);
	munmap(private, page);
	munmap(readonly, 2 * page);
	munmap(second, 2 * page);
	munmap(first, page);
	close(ro);
	close(fd);
	unlink(path);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
