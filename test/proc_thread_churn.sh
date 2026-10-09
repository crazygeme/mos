#!/bin/sh
# Validate proc task enumeration during concurrent thread creation and exit.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_thread_churn.XXXXXX)
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

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int done;
static void *scan(void *unused)
{
	struct stat st;
	int finish;
	for (;;) {
		pthread_mutex_lock(&lock);
		finish = done;
		pthread_mutex_unlock(&lock);
		if (finish)
			break;
		CHECK(names("/proc/self/task", 0, 1) > 0);
		CHECK(!stat("/proc/self/task", &st));
	}
	return 0;
}
static void *nothing(void *unused)
{
	return 0;
}
int main(void)
{
	pthread_t scanner, t[8];
	int i, j;
	alarm(30);
	CHECK(!pthread_create(&scanner, 0, scan, 0));
	for (i = 0; i < 100; i++) {
		for (j = 0; j < 8; j++)
			CHECK(!pthread_create(&t[j], 0, nothing, 0));
		for (j = 0; j < 8; j++)
			CHECK(!pthread_join(t[j], 0));
	}
	pthread_mutex_lock(&lock);
	done = 1;
	pthread_mutex_unlock(&lock);
	CHECK(!pthread_join(scanner, 0));
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
