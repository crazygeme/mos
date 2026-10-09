#!/bin/sh
# Validate descriptor-directory reopening and Bash process substitution.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-dev_fd.XXXXXX)
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

#include <dirent.h>
static int names(const char *path, const char *wanted, int numeric)
{
	DIR *d = opendir(path);
	struct dirent *e;
	int count = 0, found = 0;
	CHECK(d);
	while ((e = readdir(d))) {
		const char *p = e->d_name;
		if (!strcmp(p, ".") || !strcmp(p, ".."))
			continue;
		count++;
		if (wanted && !strcmp(p, wanted))
			found = 1;
		if (numeric) {
			CHECK(*p);
			while (*p)
				CHECK(*p >= '0' && *p++ <= '9');
		}
	}
	closedir(d);
	if (wanted)
		CHECK(found);
	return count;
}

int main(void)
{
	const char *dirs[] = { "/dev/fd", "/proc/self/fd" };
	int i, p[2], reopened, fd;
	char link[64], number[32], b[8];
	struct stat st;
	alarm(10);
	for (i = 0; i < 2; i++) {
		CHECK(!pipe(p));
		snprintf(link, sizeof(link), "%s/%d", dirs[i], p[1]);
		CHECK(!lstat(link, &st) && S_ISLNK(st.st_mode));
		CHECK(!stat(link, &st) && S_ISFIFO(st.st_mode));
		snprintf(number, sizeof(number), "%d", p[1]);
		names(dirs[i], number, 0);
		CHECK((reopened = open(link, O_WRONLY | O_NONBLOCK)) >= 0);
		CHECK(!(fcntl(p[1], F_GETFL) & O_NONBLOCK) &&
		      (fcntl(reopened, F_GETFL) & O_NONBLOCK));
		CHECK(!fstat(reopened, &st) && st.st_uid == geteuid());
		close(p[1]);
		CHECK(write(reopened, "pipe", 4) == 4 &&
		      read(p[0], b, 4) == 4 && !memcmp(b, "pipe", 4));
		CHECK(!fcntl(p[0], F_SETFL, O_NONBLOCK));
		CHECK(read(p[0], b, 1) == -1 && errno == EAGAIN);
		close(reopened);
		CHECK(read(p[0], b, 1) == 0);
		close(p[0]);
		CHECK(!pipe(p));
		snprintf(link, sizeof(link), "%s/%d", dirs[i], p[0]);
		CHECK((reopened = open(link, O_RDONLY)) >= 0);
		close(p[0]);
		CHECK(write(p[1], "reader", 6) == 6 &&
		      read(reopened, b, 6) == 6 && !memcmp(b, "reader", 6));
		close(p[1]);
		close(reopened);
		CHECK(!pipe(p));
		snprintf(link, sizeof(link), "%s/%d", dirs[i], p[0]);
		CHECK((reopened = open(link, O_RDWR)) >= 0);
		close(p[0]);
		close(p[1]);
		CHECK(write(reopened, "both", 4) == 4 &&
		      read(reopened, b, 4) == 4 && !memcmp(b, "both", 4));
		close(reopened);
		fd = temp_file();
		CHECK(write(fd, "file", 4) == 4);
		snprintf(link, sizeof(link), "%s/%d", dirs[i], fd);
		CHECK((reopened = open(link, O_RDONLY)) >= 0);
		CHECK(read(reopened, b, 4) == 4 && !memcmp(b, "file", 4));
		CHECK(lseek(fd, 0, SEEK_CUR) == 4);
		close(reopened);
		close(fd);
	}
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
/bin/bash -c 'exec > >(exec cat); exec 2> >(exec cat >&2); printf "stdout\n"; printf "stderr\n" >&2' > "$probe_dir/out" 2> "$probe_dir/err"
printf 'stdout\n' > "$probe_dir/expected-out"
printf 'stderr\n' > "$probe_dir/expected-err"
cmp "$probe_dir/out" "$probe_dir/expected-out"
cmp "$probe_dir/err" "$probe_dir/expected-err"
/bin/bash -c 'cat <(printf "input\n")' > "$probe_dir/out" 2> "$probe_dir/err"
printf 'input\n' > "$probe_dir/expected-out"
cmp "$probe_dir/out" "$probe_dir/expected-out"
[ ! -s "$probe_dir/err" ]
