#!/bin/sh
# Validate procfs thread-group directories and live thread metadata.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_tasks.XXXXXX)
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
static long field(const char *path, const char *key)
{
	FILE *f = fopen(path, "r");
	char line[1024], name[80];
	long n;
	CHECK(f);
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%79[^:]: %ld", name, &n) == 2 &&
		    !strcmp(name, key)) {
			fclose(f);
			return n;
		}
	}
	fclose(f);
	CHECK(0);
	return -1;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready, finish;
static pid_t tid, pid;
static void *worker(void *unused)
{
	CHECK(field("/proc/self/status", "Pid") == pid &&
	      field("/proc/self/status", "Tgid") == pid);
	pthread_mutex_lock(&lock);
	tid = syscall(SYS_gettid);
	ready = 1;
	pthread_cond_broadcast(&cond);
	while (!finish)
		pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);
	return 0;
}
static void directory(const char *p, int count)
{
	struct stat st;
	CHECK(!stat(p, &st) && S_ISDIR(st.st_mode) && st.st_nlink == count + 2);
	CHECK(names(p, 0, 1) == count);
}
int main(void)
{
	int procfd, taskfd, base, p[2], d[2];
	pid_t child;
	pthread_t t;
	struct stat st;
	char path[128], entry[128], exe[4096], other[4096], c;
	ssize_t n, m;
	alarm(10);
	pid = getpid();
	snprintf(path, sizeof(path), "/proc/%d/task", pid);
	base = names(path, 0, 1);
	directory("/proc/self/task", base);
	names("/proc/self", "task", 0);
	CHECK((procfd = open("/proc", O_RDONLY | O_DIRECTORY)) >= 0);
	CHECK((taskfd = open(path, O_RDONLY | O_DIRECTORY)) >= 0);
	CHECK(!pthread_create(&t, 0, worker, 0));
	pthread_mutex_lock(&lock);
	while (!ready)
		pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);
	directory(path, base + 1);
	directory("/proc/self/task", base + 1);
	CHECK(!syscall(300, procfd, "self/task/", &st, 0) &&
	      st.st_nlink == base + 3);
	CHECK(!fstat(taskfd, &st) && st.st_nlink == base + 3);
	snprintf(entry, sizeof(entry), "%s/%d", path, tid);
	CHECK(!stat(entry, &st) && S_ISDIR(st.st_mode));
	{
		DIR *d = opendir(entry);
		struct dirent *e;
		CHECK(d);
		while ((e = readdir(d)))
			CHECK(strcmp(e->d_name, "task"));
		closedir(d);
	}
	strcat(entry, "/status");
	CHECK(field(entry, "Pid") == tid && field(entry, "Tgid") == pid &&
	      field(entry, "Threads") == base + 1);
	snprintf(entry, sizeof(entry), "%s/%d/exe", path, tid);
	n = readlink(entry, exe, sizeof(exe));
	m = readlink("/proc/self/exe", other, sizeof(other));
	CHECK(n > 0 && n == m && !memcmp(exe, other, n));
	CHECK(field("/proc/self/status", "Threads") == base + 1);
	pthread_mutex_lock(&lock);
	finish = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
	CHECK(!pthread_join(t, 0));
	close(procfd);
	directory(path, base);
	CHECK(!fstat(taskfd, &st) && st.st_nlink == base + 2);
	snprintf(entry, sizeof(entry), "%s/%d", path, tid);
	CHECK(stat(entry, &st) == -1 && errno == ENOENT);
	close(taskfd);
	CHECK(!pipe(p) && !pipe(d));
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(p[0]);
		close(d[1]);
		directory("/proc/self/task", 1);
		CHECK(write(p[1], "R", 1) == 1);
		read(d[0], &c, 1);
		_exit(0);
	}
	close(p[1]);
	close(d[0]);
	CHECK(read(p[0], &c, 1) == 1);
	snprintf(entry, sizeof(entry), "%s/%d", path, child);
	CHECK(stat(entry, &st) == -1 && errno == ENOENT);
	snprintf(entry, sizeof(entry), "/proc/%d/task", child);
	directory(entry, 1);
	close(p[0]);
	close(d[1]);
	child_ok(child);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
