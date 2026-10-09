#!/bin/sh
# Validate the published PS/2 mouse endpoint without consuming input data.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-input_device_open.XXXXXX)
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

#include <dirent.h>
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

struct event {
	int wd;
	uint32_t mask, cookie, len;
};
int main(void)
{
	struct stat st;
	int fd, notify, wd, n, offset, opened = 0, closed = 0;
	char b[4096];
	struct event *e;
	CHECK(!stat("/dev/input", &st) && S_ISDIR(st.st_mode));
	CHECK(!stat("/dev/input/mice", &st) && S_ISCHR(st.st_mode));
	CHECK(major(st.st_rdev) == 13 && minor(st.st_rdev) == 63);
	{
		DIR *d = opendir("/dev");
		struct dirent *entry;
		int seen = 0;
		CHECK(d);
		while ((entry = readdir(d))) {
			CHECK(strcmp(entry->d_name, "input/mice"));
			if (!strcmp(entry->d_name, "input"))
				seen = 1;
		}
		CHECK(seen);
		closedir(d);
	}
	names("/dev/input", "mice", 0);
	CHECK((notify = syscall(332, O_NONBLOCK | O_CLOEXEC)) >= 0);
	CHECK((wd = syscall(292, notify, "/dev/input", 0x20 | 0x08)) >= 0);
	CHECK((fd = open("/dev/input/mice", O_RDWR | O_NONBLOCK)) >= 0);
	CHECK(!fstat(fd, &st) && S_ISCHR(st.st_mode));
	CHECK((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR &&
	      (fcntl(fd, F_GETFL) & O_NONBLOCK));
	close(fd);
	CHECK((n = read(notify, b, sizeof(b))) > 0);
	for (offset = 0; offset < n; offset += 16 + e->len) {
		e = (void *)(b + offset);
		CHECK(offset + 16 <= n && offset + 16 + e->len <= n &&
		      !(e->mask & 0x40000000));
		if (e->wd == wd && e->len && !strcmp(b + offset + 16, "mice")) {
			opened |= e->mask & 0x20;
			closed |= e->mask & 0x08;
		}
	}
	CHECK(opened && closed);
	close(notify);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
