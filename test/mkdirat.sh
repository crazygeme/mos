#!/bin/sh
# Validate the i386 mkdirat interface on persistent and runtime filesystems.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-mkdirat.XXXXXX)
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

static void make(int fd, const char *p, int mode, int error)
{
	int r = syscall(296, fd, p, mode);
	CHECK(error ? (r == -1 && errno == error) : r == 0);
}
int main(void)
{
	const char *roots[] = { "/root", "/dev/shm" };
	int i, fd, file, cwd;
	char path[128], absolute[160];
	struct stat st;
	mode_t mask;
	for (i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/mos-mkdirat.XXXXXX", roots[i]);
		CHECK(mkdtemp(path));
		CHECK((fd = open(path, O_RDONLY | O_DIRECTORY)) >= 0);
		mask = umask(027);
		make(fd, "child", 0777, 0);
		snprintf(absolute, sizeof(absolute), "%s/child", path);
		CHECK(!stat(absolute, &st) && (st.st_mode & 0777) == 0750);
		make(fd, "child", 0700, EEXIST);
		make(-1, "relative", 0700, EBADF);
		make(fd, "", 0700, ENOENT);
		make(fd, "missing/child", 0700, ENOENT);
		snprintf(absolute, sizeof(absolute), "%s/absolute", path);
		make(-1, absolute, 0700, 0);
		file = temp_file();
		make(file, "child", 0700, ENOTDIR);
		close(file);
		cwd = open(".", O_RDONLY);
		CHECK(cwd >= 0 && !chdir(path));
		make(-100, "cwd", 0700, 0);
		CHECK(!stat("cwd", &st) && S_ISDIR(st.st_mode));
		rmdir("child");
		rmdir("absolute");
		rmdir("cwd");
		CHECK(!fchdir(cwd));
		close(cwd);
		close(fd);
		umask(mask);
		CHECK(!rmdir(path));
	}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
