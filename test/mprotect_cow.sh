#!/bin/sh
# Verify mapping isolation and shared writes after mprotect transitions.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-mprotect_cow.XXXXXX)
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

static unsigned char *map(int fd, int flags, int prot)
{
	void *p = mmap(0, getpagesize(), prot,
		       flags | (fd < 0 ? MAP_ANONYMOUS : 0), fd, 0);
	CHECK(p != MAP_FAILED);
	return p;
}
int main(void)
{
	int fd = temp_file();
	size_t page = getpagesize();
	unsigned char *data = malloc(page), *p, *o, *fresh, c;
	pid_t child;
	CHECK(data);
	memset(data, 'A', page);
	CHECK(write(fd, data, page) == page);
	free(data);
	p = map(fd, MAP_PRIVATE, 1);
	o = map(fd, MAP_PRIVATE, 1);
	CHECK(*p == 'A' && *o == 'A');
	CHECK(!mprotect(p, page, 3));
	*p = 'B';
	CHECK(!mprotect(p, page, 1));
	CHECK(*o == 'A' && pread(fd, &c, 1, 0) == 1 && c == 'A');
	fresh = map(fd, MAP_PRIVATE, 1);
	CHECK(*fresh == 'A');
	munmap(fresh, page);
	munmap(p, page);
	munmap(o, page);
	p = map(-1, MAP_PRIVATE, 1);
	o = map(-1, MAP_PRIVATE, 1);
	CHECK(!*p && !*o);
	CHECK(!mprotect(p, page, 3));
	*p = 123;
	CHECK(!*o);
	fresh = map(-1, MAP_PRIVATE, 1);
	CHECK(!*fresh);
	munmap(fresh, page);
	munmap(p, page);
	munmap(o, page);
	p = map(-1, MAP_PRIVATE, 3);
	*p = 45;
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		CHECK(!mprotect(p, page, 0) && !mprotect(p, page, 3));
		*p = 67;
		CHECK(*p == 67);
		_exit(0);
	}
	child_ok(child);
	CHECK(*p == 45);
	munmap(p, page);
	p = map(fd, MAP_SHARED, 1);
	o = map(fd, MAP_SHARED, 1);
	CHECK(*p == 'A' && *o == 'A');
	CHECK(!mprotect(p, page, 3));
	*p = 'B';
	CHECK(*o == 'B');
	munmap(p, page);
	munmap(o, page);
	close(fd);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
