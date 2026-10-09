#!/bin/sh
# Validate page faults concurrent with neighboring VMA protection changes.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-mapping_fault_race.XXXXXX)
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

static unsigned char *base;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready;
static void *run(void *arg)
{
	int index = (int)(intptr_t)arg, i;
	size_t page = getpagesize();
	pthread_mutex_lock(&lock);
	if (++ready == 5)
		pthread_cond_broadcast(&cond);
	while (ready < 5)
		pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);
	for (i = 0; i < 2000; i++) {
		if (index < 4) {
			CHECK(!madvise(base + index * page, page,
				       MADV_DONTNEED));
			memset(base + index * page, index + 1, page);
			CHECK(base[index * page] == index + 1);
		} else {
			CHECK(!mprotect(base + 4 * page, page, 1));
			CHECK(!mprotect(base + 4 * page, page, 3));
		}
	}
	return 0;
}
int main(void)
{
	pthread_t threads[5];
	int i;
	alarm(30);
	base = mmap(0, 5 * getpagesize(), 3, MAP_PRIVATE | MAP_ANONYMOUS, -1,
		    0);
	CHECK(base != MAP_FAILED);
	for (i = 0; i < 5; i++)
		CHECK(!pthread_create(&threads[i], 0, run,
				      (void *)(intptr_t)i));
	for (i = 0; i < 5; i++)
		CHECK(!pthread_join(threads[i], 0));
	munmap(base, 5 * getpagesize());
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
