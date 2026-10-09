#!/bin/sh
# Validate Unix peer credential snapshots and short option buffers.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_peercred.XXXXXX)
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

static void cred(int fd, pid_t pid, uid_t uid, gid_t gid)
{
	struct ucred c;
	socklen_t n = sizeof(c);
	CHECK(!getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c, &n));
	CHECK(n == sizeof(c) && c.pid == pid && c.uid == uid && c.gid == gid);
}
int main(void)
{
	int s[2], fd, i, p[2], client;
	pid_t parent = getpid(), child;
	uid_t uid = geteuid();
	gid_t gid = getegid();
	socklen_t n;
	pid_t shortpid;
	struct sockaddr_un addr;
	char dir[] = "/tmp/mos-peercred.XXXXXX", b[5];
	alarm(10);
	CHECK((fd = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
	cred(fd, 0, (uid_t)-1, (gid_t)-1);
	close(fd);
	for (i = 0; i < 2; i++) {
		CHECK(!socketpair(AF_UNIX, i ? SOCK_DGRAM : SOCK_STREAM, 0, s));
		cred(s[0], parent, uid, gid);
		cred(s[1], parent, uid, gid);
		n = sizeof(shortpid);
		CHECK(!getsockopt(s[0], SOL_SOCKET, SO_PEERCRED, &shortpid,
				  &n) &&
		      shortpid == parent);
		close(s[1]);
		cred(s[0], parent, uid, gid);
		close(s[0]);
	}
	CHECK(mkdtemp(dir));
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/socket", dir);
	CHECK((fd = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
	CHECK(!bind(fd, (void *)&addr, sizeof(addr)) && !listen(fd, 1));
	CHECK(!pipe(p));
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(fd);
		close(p[0]);
		CHECK((client = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
		CHECK(!connect(client, (void *)&addr, sizeof(addr)));
		cred(client, parent, uid, gid);
		if (!uid) {
			CHECK(!setgid(65534) && !setuid(65534));
		}
		CHECK(write(p[1], "ready", 5) == 5);
		CHECK(read(client, b, 2) == 2 && !memcmp(b, "ok", 2));
		close(client);
		_exit(0);
	}
	close(p[1]);
	CHECK((client = accept(fd, 0, 0)) >= 0);
	CHECK(read(p[0], b, 5) == 5);
	cred(client, child, uid, gid);
	CHECK(write(client, "ok", 2) == 2);
	CHECK(read(client, b, 1) == 0);
	cred(client, child, uid, gid);
	child_ok(child);
	close(client);
	close(fd);
	close(p[0]);
	unlink(addr.sun_path);
	rmdir(dir);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
