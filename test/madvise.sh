#!/bin/sh
# Validate page discard, mapping preservation, and native-width lengths.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-madvise.XXXXXX)
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

int main(void)
{
	size_t page = getpagesize(), i;
	unsigned char *p =
		mmap(0, 3 * page, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int fd;
	pid_t pid;
	CHECK(p != MAP_FAILED);
	memset(p, 'A', 3 * page);
	CHECK(!madvise(p + page, 1, MADV_DONTNEED));
	for (i = 0; i < 3 * page; i++)
		CHECK(p[i] == (i >= page && i < 2 * page ? 0 : 'A'));
	p[page] = 7;
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		CHECK(!madvise(p + page, page, MADV_DONTNEED));
		CHECK(p[page] == 0);
		_exit(0);
	}
	child_ok(pid);
	CHECK(p[page] == 7);
	CHECK(madvise(p + 1, page, MADV_DONTNEED) == -1 && errno == EINVAL);
	munmap(p, 3 * page);
	fd = temp_file();
	p = malloc(page);
	CHECK(p);
	memset(p, 'F', page);
	CHECK(write(fd, p, page) == page);
	free(p);
	p = mmap(0, page, 3, MAP_PRIVATE, fd, 0);
	CHECK(p != MAP_FAILED);
	p[0] = 9;
	CHECK(!madvise(p, page, MADV_DONTNEED));
	for (i = 0; i < page; i++)
		CHECK(p[i] == 'F');
	munmap(p, page);
	close(fd);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
