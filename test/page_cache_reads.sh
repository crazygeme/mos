#!/bin/sh
# Validate concurrent file-backed faults and descriptor offset preservation.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-page_cache_reads.XXXXXX)
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

static unsigned char *view;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready;
static void content(unsigned char *p, int index)
{
	uint64_t word[2];
	size_t n;
	word[0] = index;
	word[1] = index ^ UINT64_C(0x9E3779B97F4A7C15);
	for (n = 0; n < 4096; n += 16)
		memcpy(p + n, word, 16);
}
static void *worker(void *arg)
{
	unsigned char expected[4096];
	int index = (int)(intptr_t)arg, k, indices[512], i, j, tmp;
	unsigned seed = index + 1;
	for (k = 0; k < 512; k++)
		indices[k] = index + k * 8;
	for (i = 511; i > 0; i--) {
		j = rand_r(&seed) % (i + 1);
		tmp = indices[i];
		indices[i] = indices[j];
		indices[j] = tmp;
	}
	pthread_mutex_lock(&lock);
	if (++ready == 8)
		pthread_cond_broadcast(&cond);
	while (ready < 8)
		pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);
	for (k = 0; k < 512; k++) {
		content(expected, indices[k]);
		CHECK(!memcmp(view + indices[k] * 4096, expected, 4096));
	}
	return 0;
}
int main(int argc, char **argv)
{
	char path[4096];
	int fd, i;
	unsigned char b[4096];
	pthread_t t[8];
	alarm(60);
	snprintf(path, sizeof(path), "%s/mos-pages.XXXXXX",
		 argc == 3 && !strcmp(argv[1], "--directory") ? argv[2] : ".");
	CHECK((fd = mkstemp(path)) >= 0);
	unlink(path);
	for (i = 0; i < 4096; i++) {
		content(b, i);
		CHECK(write(fd, b, sizeof(b)) == sizeof(b));
	}
	CHECK(lseek(fd, 0x1237, SEEK_SET) == 0x1237);
	view = mmap(0, 4096 * 4096, 3, MAP_PRIVATE, fd, 0);
	CHECK(view != MAP_FAILED);
	for (i = 0; i < 8; i++)
		CHECK(!pthread_create(&t[i], 0, worker, (void *)(intptr_t)i));
	for (i = 0; i < 8; i++)
		CHECK(!pthread_join(t[i], 0));
	CHECK(lseek(fd, 0, SEEK_CUR) == 0x1237);
	munmap(view, 4096 * 4096);
	close(fd);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
