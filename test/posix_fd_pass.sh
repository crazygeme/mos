#!/bin/sh

set -e
CASE_NAME=posix_fd_pass
CASE_MTIME=@CASE_MTIME@
BASE=/root/tests/$CASE_NAME
WORKDIR=/root/posix_fd_pass
SRC="$BASE/fd_pass.c"
BIN="$BASE/fd_pass"
OUT="$BASE/out.txt"
STAMP="$BASE/.case_timestamp"

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

prepare_embedded_c()
{
	mkdir -p "$BASE" "$WORKDIR" >/dev/null 2>&1 || fail "mkdir failed"
	if [ -f "$STAMP" ] && [ -f "$SRC" ] && [ -f "$BIN" ]; then
		stamp_mtime=$(cat "$STAMP" 2>/dev/null || printf '')
		if [ "$stamp_mtime" = "$CASE_MTIME" ]; then
			return 1
		fi
	fi
	return 0
}

update_case_timestamp()
{
	printf '%s\n' "$CASE_MTIME" > "$STAMP" || fail "timestamp write failed"
}

finish()
{
	sync >/dev/null 2>&1 || true
}

trap finish EXIT

if prepare_embedded_c; then
cat > "$SRC" <<'EOF'
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static ssize_t retry_read_eintr(int fd, void *buf, size_t len)
{
	ssize_t n;

	do {
		n = read(fd, buf, len);
	} while (n < 0 && errno == EINTR);

	return n;
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

static void run_stream_disconnect_poll(void)
{
	int sv[2];
	struct pollfd pfd;
	char ch = 'X';
	ssize_t n;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		die("socketpair disconnect");

	if (close(sv[1]) < 0)
		die("close disconnect peer");

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = sv[0];
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 0) != 1) {
		fprintf(stderr, "disconnect poll did not become ready\n");
		exit(1);
	}
	if (!(pfd.revents & POLLHUP)) {
		fprintf(stderr, "disconnect poll missing POLLHUP: %#x\n",
			pfd.revents);
		exit(1);
	}

	n = read(sv[0], &ch, 1);
	if (n != 0) {
		fprintf(stderr, "disconnect read expected EOF, got %ld\n", (long)n);
		exit(1);
	}

	errno = 0;
	n = write(sv[0], "Q", 1);
	if (n != -1 || errno != EPIPE) {
		fprintf(stderr, "disconnect write expected EPIPE, got n=%ld errno=%d\n",
			(long)n, errno);
		exit(1);
	}

	close(sv[0]);
}

static void run_named_stream_server_exit(void)
{
	int listenfd;
	int clientfd;
	int ready_pipe[2];
	struct sockaddr_un addr;
	pid_t pid;
	int status;
	struct pollfd pfd;
	char path[256];
	char ch = 'X';
	ssize_t n;

	snprintf(path, sizeof(path), "/root/posix_fd_pass/named-exit.sock");
	unlink(path);
	if (pipe(ready_pipe) < 0)
		die("pipe named stream");

	pid = fork();
	if (pid < 0)
		die("fork named stream");

	if (pid == 0) {
		int connfd;
		char ready = 'R';

		close(ready_pipe[0]);

		listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (listenfd < 0)
			die("server socket");

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

		if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
			die("server bind");
		if (listen(listenfd, 1) < 0)
			die("server listen");
		if (write(ready_pipe[1], &ready, 1) != 1)
			die("server ready pipe");
		close(ready_pipe[1]);

		connfd = accept(listenfd, NULL, NULL);
		if (connfd < 0)
			die("server accept");

		if (write(connfd, "R", 1) != 1)
			die("server write ready");

		close(connfd);
		close(listenfd);
		_exit(0);
	}

	close(ready_pipe[1]);
	if (read(ready_pipe[0], &ch, 1) != 1)
		die("client ready pipe");
	close(ready_pipe[0]);

	clientfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (clientfd < 0)
		die("client socket");

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(clientfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("client connect");

	n = retry_read_eintr(clientfd, &ch, 1);
	if (n != 1 || ch != 'R') {
		fprintf(stderr,
			"named stream initial read mismatch: n=%ld ch=%d errno=%d\n",
			(long)n, (int)ch, errno);
		exit(1);
	}

	if (waitpid(pid, &status, 0) < 0)
		die("waitpid named stream");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "named stream server exit status %#x\n", status);
		exit(1);
	}

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = clientfd;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 0) != 1) {
		fprintf(stderr, "named stream poll did not become ready\n");
		exit(1);
	}
	if (!(pfd.revents & POLLHUP)) {
		fprintf(stderr, "named stream missing POLLHUP: %#x\n", pfd.revents);
		exit(1);
	}

	n = retry_read_eintr(clientfd, &ch, 1);
	if (n != 0) {
		fprintf(stderr, "named stream expected EOF, got %ld\n", (long)n);
		exit(1);
	}

	signal(SIGPIPE, SIG_IGN);
	errno = 0;
	n = write(clientfd, "Q", 1);
	if (n != -1 || errno != EPIPE) {
		fprintf(stderr,
			"named stream expected EPIPE, got n=%ld errno=%d\n",
			(long)n, errno);
		exit(1);
	}

	close(clientfd);
	unlink(path);
}

int main(void)
{
	run_one(SOCK_STREAM, "stream-passfd");
	run_one(SOCK_DGRAM, "dgram-passfd");
	run_stream_back_to_back();
	run_stream_disconnect_poll();
	run_named_stream_server_exit();
	return 0;
}
EOF

expect_success gcc -Wall -Werror -o "$BIN" "$SRC"
update_case_timestamp
fi

set +e
"$BIN" > "$OUT" 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || fail "Expected: success from '$BIN'
Actual: exit status $rc
Output: $(cat "$OUT" 2>/dev/null || printf '<none>')"
