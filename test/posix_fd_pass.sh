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

static void expect_payload_and_fd(int sock, char payload, const char *label)
{
	int recvfd;
	char recvbyte = 0;
	char control[CMSG_SPACE(sizeof(int))];
	char readbuf[32];
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	ssize_t n;

	memset(&msg, 0, sizeof(msg));
	memset(control, 0, sizeof(control));
	iov.iov_base = &recvbyte;
	iov.iov_len = sizeof(recvbyte);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	n = recvmsg(sock, &msg, 0);
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
}

static void send_fd_with_payload(int sock, int fd, char payload)
{
	char control[CMSG_SPACE(sizeof(int))];
	struct iovec iov;
	struct msghdr msg;
	struct cmsghdr *cmsg;

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

	if (sendmsg(sock, &msg, 0) != 1)
		die("sendmsg");
}

static void run_one(int socktype, const char *label)
{
	int sv[2];
	int fd;
	char path[256];
	char payload = 'Z';

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

	send_fd_with_payload(sv[0], fd, payload);
	if (close(fd) < 0)
		die("close send fd");
	expect_payload_and_fd(sv[1], payload, label);
	close(sv[0]);
	close(sv[1]);
}

static void run_stream_back_to_back(void)
{
	int sv[2];
	int fd1;
	int fd2;
	char path1[256];
	char path2[256];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		die("socketpair");

	snprintf(path1, sizeof(path1), "/root/posix_fd_pass/stream-a.txt");
	fd1 = open(path1, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd1 < 0)
		die("open stream-a");
	if (write(fd1, "stream-a", 8) != 8)
		die("write stream-a");
	if (lseek(fd1, 0, SEEK_SET) < 0)
		die("lseek stream-a");

	snprintf(path2, sizeof(path2), "/root/posix_fd_pass/stream-b.txt");
	fd2 = open(path2, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd2 < 0)
		die("open stream-b");
	if (write(fd2, "stream-b", 8) != 8)
		die("write stream-b");
	if (lseek(fd2, 0, SEEK_SET) < 0)
		die("lseek stream-b");

	send_fd_with_payload(sv[0], fd1, 'A');
	send_fd_with_payload(sv[0], fd2, 'B');

	close(fd1);
	close(fd2);

	expect_payload_and_fd(sv[1], 'A', "stream-a");
	expect_payload_and_fd(sv[1], 'B', "stream-b");

	close(sv[0]);
	close(sv[1]);
}

int main(void)
{
	run_one(SOCK_STREAM, "stream-passfd");
	run_one(SOCK_DGRAM, "dgram-passfd");
	run_stream_back_to_back();
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
