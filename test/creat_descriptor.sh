#!/bin/sh
# Verify creat returns a writable descriptor and truncates relative paths.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-creat_descriptor.XXXXXX)
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
	int held[3], fd, i;
	struct stat st;
	char b[16];
	for (i = 0; i < 3; i++)
		CHECK((held[i] = open("/dev/null", O_RDONLY)) >= 0);
	CHECK((fd = creat("created.txt", 0600)) >= 0);
	for (i = 0; i < 3; i++)
		CHECK(fd != held[i]);
	CHECK(write(fd, "created payload", 15) == 15);
	close(fd);
	CHECK((fd = open("created.txt", O_RDONLY)) >= 0);
	CHECK(read(fd, b, 16) == 15 && !memcmp(b, "created payload", 15));
	close(fd);
	CHECK((fd = creat("created.txt", 0600)) >= 0);
	CHECK(!fstat(fd, &st) && st.st_size == 0);
	close(fd);
	unlink("created.txt");
	for (i = 0; i < 3; i++) {
		CHECK(!fstat(held[i], &st));
		close(held[i]);
	}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
