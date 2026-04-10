#!/bin/sh

set -e
BASE=/root/posix_fd_pass
SRC="$BASE/fd_pass.c"
BIN="$BASE/fd_pass"
OUT="$BASE/out.txt"

fail()
{
	echo "posix_fd_pass: $1" >&2
	exit 1
}

expect_success()
{
	set +e
	output=$("$@" 2>&1)
	rc=$?
	set -e
	[ "$rc" -eq 0 ] && return 0
	fail "Expected: success from '$*'
Actual: exit status $rc
Output: ${output:-<none>}"
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"

cat > "$SRC" <<'EOF'
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static void run_one(int socktype, const char *label)
{
	int sv[2];
	int fd;
	int recvfd;
	char path[256];
	char payload = 'Z';
	char recvbyte = 0;
	char control[CMSG_SPACE(sizeof(int))];
	char readbuf[32];
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	ssize_t n;

	if (socketpair(AF_UNIX, socktype, 0, sv) < 0)
		die("socketpair");

	snprintf(path, sizeof(path), "/root/posix_fd_pass/%s.txt", label);
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0)
		die("open");
	if (write(fd, label, strlen(label)) != (ssize_t)strlen(label))
		die("write");
	if (lseek(fd, 0, SEEK_SET) < 0)
		die("lseek");

	memset(&msg, 0, sizeof(msg));
	memset(control, 0, sizeof(control));
	iov.iov_base = &payload;
	iov.iov_len = sizeof(payload);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

	if (sendmsg(sv[0], &msg, 0) != 1)
		die("sendmsg");
	if (close(fd) < 0)
		die("close send fd");

	memset(&msg, 0, sizeof(msg));
	memset(control, 0, sizeof(control));
	iov.iov_base = &recvbyte;
	iov.iov_len = sizeof(recvbyte);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	n = recvmsg(sv[1], &msg, 0);
	if (n != 1) {
		fprintf(stderr, "recvmsg returned %ld for %s\n", (long)n, label);
		exit(1);
	}
	if (recvbyte != payload) {
		fprintf(stderr, "payload mismatch for %s: %d != %d\n", label,
			(int)recvbyte, (int)payload);
		exit(1);
	}
	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg) {
		fprintf(stderr, "missing cmsg for %s\n", label);
		exit(1);
	}
	if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
		fprintf(stderr, "bad cmsg header for %s\n", label);
		exit(1);
	}
	memcpy(&recvfd, CMSG_DATA(cmsg), sizeof(recvfd));

	memset(readbuf, 0, sizeof(readbuf));
	n = read(recvfd, readbuf, sizeof(readbuf) - 1);
	if (n != (ssize_t)strlen(label)) {
		fprintf(stderr, "read length mismatch for %s: %ld\n", label,
			(long)n);
		exit(1);
	}
	readbuf[n] = '\0';
	if (strcmp(readbuf, label) != 0) {
		fprintf(stderr, "read mismatch for %s: %s\n", label, readbuf);
		exit(1);
	}

	close(recvfd);
	close(sv[0]);
	close(sv[1]);
}

int main(void)
{
	run_one(SOCK_STREAM, "stream-passfd");
	run_one(SOCK_DGRAM, "dgram-passfd");
	return 0;
}
EOF

expect_success gcc -Wall -Werror -o "$BIN" "$SRC"

set +e
"$BIN" > "$OUT" 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || fail "Expected: success from '$BIN'
Actual: exit status $rc
Output: $(cat "$OUT" 2>/dev/null || printf '<none>')"
