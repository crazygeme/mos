#!/bin/sh
# Validate procfs working-directory links; --tmux also checks pane paths.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_cwd.XXXXXX)
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

int main(void)
{
	char cwd[4096], tmp[] = "/tmp/mos-cwd.XXXXXX", deep[4096], link[64], c;
	const char *paths[3];
	int i, ready[2], done[2];
	pid_t pid;
	alarm(10);
	CHECK(getcwd(cwd, sizeof(cwd)));
	names("/proc", "self", 0);
	snprintf(link, sizeof(link), "/proc/%d", getpid());
	CHECK(names("/proc/self", 0, 0) == names(link, 0, 0));
	CHECK(mkdtemp(tmp));
	snprintf(deep, sizeof(deep), "%s/one", tmp);
	CHECK(!mkdir(deep, 0700));
	strcat(deep, "/two");
	CHECK(!mkdir(deep, 0700));
	strcat(deep, "/three");
	CHECK(!mkdir(deep, 0700));
	paths[0] = "/";
	paths[1] = tmp;
	paths[2] = deep;
	for (i = 0; i < 3; i++) {
		CHECK(!chdir(paths[i]));
		link_check("/proc/self/cwd", paths[i], 0);
		snprintf(link, sizeof(link), "/proc/%d/cwd", getpid());
		link_check(link, paths[i], 0);
	}
	CHECK(!chdir(cwd));
	CHECK(!pipe(ready) && !pipe(done));
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		close(ready[0]);
		close(done[1]);
		CHECK(!chdir(deep));
		CHECK(write(ready[1], "R", 1) == 1);
		read(done[0], &c, 1);
		_exit(0);
	}
	close(ready[1]);
	close(done[0]);
	CHECK(read(ready[0], &c, 1) == 1 && c == 'R');
	snprintf(link, sizeof(link), "/proc/%d/cwd", pid);
	link_check(link, deep, 0);
	close(done[1]);
	close(ready[0]);
	child_ok(pid);
	rmdir(deep);
	*strrchr(deep, '/') = 0;
	rmdir(deep);
	*strrchr(deep, '/') = 0;
	rmdir(deep);
	rmdir(tmp);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
if [ "${1:-}" = --tmux ]; then
    mkdir -p "$probe_dir/one/two/three"
    tmux -S "$probe_dir/server" -f /dev/null new-session -d -s cwd-check -c "$probe_dir/one/two/three" 'sleep 30'
    actual=$(tmux -S "$probe_dir/server" display-message -p -t cwd-check:0.0 '#{pane_current_path}')
    tmux -S "$probe_dir/server" kill-server
    [ "$actual" = "$probe_dir/one/two/three" ]
fi
