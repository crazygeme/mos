#!/bin/sh
# Validate procfs executable links and readlinkat in the running system.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_exe.XXXXXX)
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

static void link_check(const char *link, const char *expected, int at)
{
	struct stat a, b;
	char buf[4097], parent[4096], *slash;
	int fd;
	size_t len = strlen(expected), n, i, capacity;
	CHECK(!lstat(link, &a) && S_ISLNK(a.st_mode));
	CHECK(!stat(link, &a) && !stat(expected, &b) && a.st_ino == b.st_ino);
	strcpy(parent, link);
	slash = strrchr(parent, '/');
	*slash = 0;
	names(parent, slash + 1, 0);
	for (capacity = 2; capacity <= 4096; capacity *= 2048) {
		memset(buf, 'X', sizeof(buf));
		n = len < capacity ? len : capacity;
		CHECK(readlink(link, buf, capacity) == n &&
		      !memcmp(buf, expected, n));
		for (i = n; i < capacity + 1; i++)
			CHECK(buf[i] == 'X');
		if (at) {
			CHECK(syscall(305, -100, link, buf, capacity) == n &&
			      !memcmp(buf, expected, n));
		}
	}
	if (at) {
		CHECK((fd = open(parent, O_RDONLY | O_DIRECTORY)) >= 0);
		CHECK(syscall(305, fd, slash + 1, buf, sizeof(buf)) == len &&
		      !memcmp(buf, expected, len));
		close(fd);
	}
}

static void self(const char *expected)
{
	char p[64];
	link_check("/proc/self/exe", expected, 1);
	snprintf(p, sizeof(p), "/proc/%d/exe", getpid());
	link_check(p, expected, 1);
}
int main(int argc, char **argv)
{
	char expected[4096], tmp[] = "/tmp/mos-exe.XXXXXX", path[4096],
			     link[64], c;
	int fd, ready[2], done[2];
	pid_t pid;
	ssize_t n;
	alarm(10);
	if (argc == 3) {
		CHECK(!chdir("/"));
		self(argv[2]);
		return 0;
	}
	n = readlink("/proc/self/exe", expected, sizeof(expected) - 1);
	CHECK(n > 0);
	expected[n] = 0;
	self(expected);
	CHECK(mkdtemp(tmp));
	snprintf(path, sizeof(path), "%s/invalid", tmp);
	CHECK((fd = open(path, O_CREAT | O_WRONLY, 0700)) >= 0);
	CHECK(write(fd, "invalid executable\n", 19) == 19);
	close(fd);
	{
		char *args[] = { "unrelated-name", 0 };
		execv(path, args);
		CHECK(errno == ENOEXEC);
		self(expected);
		snprintf(path, sizeof(path), "%s/missing", tmp);
		execv(path, args);
		CHECK(errno == ENOENT);
		self(expected);
	}
	snprintf(path, sizeof(path), "%s/probe-alias", tmp);
	CHECK(!symlink(expected, path));
	CHECK(!pipe(ready) && !pipe(done));
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		close(ready[0]);
		close(done[1]);
		CHECK(!chdir(tmp));
		self(expected);
		CHECK(write(ready[1], "R", 1) == 1);
		read(done[0], &c, 1);
		close(ready[1]);
		close(done[0]);
		execl("./probe-alias", "unrelated-name", "--exec-check",
		      expected, (char *)0);
		_exit(1);
	}
	close(ready[1]);
	close(done[0]);
	CHECK(read(ready[0], &c, 1) == 1);
	snprintf(link, sizeof(link), "/proc/%d/exe", pid);
	link_check(link, expected, 1);
	close(ready[0]);
	close(done[1]);
	child_ok(pid);
	unlink(path);
	snprintf(path, sizeof(path), "%s/invalid", tmp);
	unlink(path);
	rmdir(tmp);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
