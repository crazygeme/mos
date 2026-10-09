#!/bin/sh
# Exercise shared descriptor tables, fork isolation, and inotify lifetime.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-shared_fds.XXXXXX)
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

#include <sched.h>
static int p[2], fd, action;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready, created;
static void *worker(void *unused)
{
	char b[8];
	if (action == 0) {
		CHECK(!pipe(p));
		CHECK(write(p[1], "worker", 6) == 6);
	} else if (action == 1) {
		pthread_mutex_lock(&lock);
		ready = 1;
		pthread_cond_broadcast(&cond);
		while (!created)
			pthread_cond_wait(&cond, &lock);
		pthread_mutex_unlock(&lock);
		CHECK(read(p[0], b, 4) == 4 && !memcmp(b, "data", 4));
	} else if (action == 2)
		CHECK(!fcntl(fd, F_SETFD, FD_CLOEXEC));
	else
		CHECK(!close(fd));
	return 0;
}
static int exec_child(void *unused)
{
	execl("/bin/true", "true", (char *)0);
	return 127;
}
int main(void)
{
	pthread_t t;
	char b[8];
	pid_t pid;
	int i, j, other[32];
	struct pollfd poller;
	void *stack;
	alarm(20);
	action = 0;
	CHECK(!pthread_create(&t, 0, worker, 0));
	CHECK(!pthread_join(t, 0));
	CHECK(read(p[0], b, 6) == 6 && !memcmp(b, "worker", 6));
	CHECK(write(p[1], "main", 4) == 4 && read(p[0], b, 4) == 4 &&
	      !memcmp(b, "main", 4));
	close(p[0]);
	close(p[1]);
	action = 1;
	CHECK(!pthread_create(&t, 0, worker, 0));
	pthread_mutex_lock(&lock);
	while (!ready)
		pthread_cond_wait(&cond, &lock);
	CHECK(!pipe(p) && write(p[1], "data", 4) == 4);
	created = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
	CHECK(!pthread_join(t, 0));
	close(p[0]);
	close(p[1]);
	CHECK((fd = open("/dev/null", O_RDONLY)) >= 0);
	action = 2;
	CHECK(!pthread_create(&t, 0, worker, 0) && !pthread_join(t, 0));
	CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
	action = 3;
	CHECK(!pthread_create(&t, 0, worker, 0) && !pthread_join(t, 0));
	CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
	CHECK((fd = open("/dev/null", O_RDONLY)) >= 0);
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		close(fd);
		_exit(0);
	}
	child_ok(pid);
	CHECK(read(fd, b, 1) == 0);
	CHECK(!fcntl(fd, F_SETFD, FD_CLOEXEC));
	stack = malloc(256 * 1024);
	CHECK(stack);
	pid = clone(exec_child, (char *)stack + 256 * 1024,
		    CLONE_FILES | SIGCHLD, 0);
	child_ok(pid);
	CHECK(read(fd, b, 1) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
	close(fd);
	free(stack);
	fd = syscall(332, O_NONBLOCK | O_CLOEXEC);
	CHECK(fd >= 0);
	for (i = 0; i < 2; i++) {
		pid = fork();
		CHECK(pid >= 0);
		if (!pid) {
			if (i)
				execl("/bin/true", "true", (char *)0);
			close(fd);
			_exit(i ? 127 : 0);
		}
		child_ok(pid);
		for (j = 0; j < 32; j++)
			CHECK((other[j] = open("/dev/null", O_RDONLY)) >= 0);
		poller.fd = fd;
		poller.events = POLLIN;
		CHECK(!poll(&poller, 1, 0));
		CHECK(read(fd, b, sizeof(b)) == -1 && errno == EAGAIN);
		for (j = 0; j < 32; j++)
			close(other[j]);
	}
	close(fd);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
