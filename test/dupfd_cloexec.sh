#!/bin/sh
# Verify F_DUPFD_CLOEXEC allocation, file sharing, and exec semantics.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-dupfd_cloexec.XXXXXX)
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

int main(int argc, char **argv)
{
	int fd, occupied, duplicate, replacement;
	char c, number[32];
	struct rlimit lim;
	pid_t pid;
	if (argc == 2) {
		CHECK(fcntl(atoi(argv[1]), F_GETFD) == -1 && errno == EBADF);
		return 0;
	}
	fd = temp_file();
	CHECK(write(fd, "abcd", 4) == 4);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(!fcntl(fd, F_SETFD, 0));
	CHECK((occupied = fcntl(fd, F_DUPFD, 64)) >= 64);
	CHECK((duplicate = fcntl(fd, F_DUPFD_CLOEXEC, occupied)) > occupied);
	CHECK(fcntl(duplicate, F_GETFD) == FD_CLOEXEC);
	CHECK(!fcntl(fd, F_GETFD) && !fcntl(occupied, F_GETFD));
	CHECK(read(duplicate, &c, 1) == 1 && c == 'a');
	CHECK(read(fd, &c, 1) == 1 && c == 'b');
	close(occupied);
	CHECK((replacement = fcntl(fd, F_DUPFD_CLOEXEC, 64)) < duplicate &&
	      replacement >= 64);
	close(replacement);
	snprintf(number, sizeof(number), "%d", duplicate);
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		execl(argv[0], argv[0], number, (char *)0);
		_exit(2);
	}
	child_ok(pid);
	close(duplicate);
	CHECK(fcntl(0x7fffffff, F_DUPFD_CLOEXEC, 0) == -1 && errno == EBADF);
	CHECK(!getrlimit(RLIMIT_NOFILE, &lim));
	CHECK(fcntl(fd, F_DUPFD_CLOEXEC, -1) == -1 && errno == EINVAL);
	CHECK(fcntl(fd, F_DUPFD_CLOEXEC, lim.rlim_cur) == -1 &&
	      errno == EINVAL);
	close(fd);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
