#!/bin/sh

set -e
CASE_NAME=posix_socket
CASE_MTIME=@CASE_MTIME@
BASE=/root/tests/$CASE_NAME
WORKDIR=/root/tests/$CASE_NAME
SRC="$BASE/socket_endpoints.c"
BIN="$BASE/socket_endpoints"
STAMP="$BASE/.case_timestamp"

fail()
{
	echo "posix_socket_endpoints: $1" >&2
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

require_cmd()
{
	command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
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

require_cmd gcc

if prepare_embedded_c; then
cat > "$SRC" <<'EOF'
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static void failf(const char *label, const char *detail)
{
	fprintf(stderr, "%s: %s\n", label, detail);
	exit(1);
}

static void expect_errno_is(int actual, int want1, int want2,
			    const char *label)
{
	if (actual == want1 || (want2 >= 0 && actual == want2))
		return;
	fprintf(stderr, "%s: expected errno %d", label, want1);
	if (want2 >= 0)
		fprintf(stderr, " or %d", want2);
	fprintf(stderr, ", got %d\n", actual);
	exit(1);
}

static void set_nonblock(int fd, int on, const char *label)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0)
		die(label);
	if (on)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
		die(label);
}

static void wait_child_ok(pid_t pid, const char *label)
{
	int status;

	if (waitpid(pid, &status, 0) < 0)
		die("waitpid");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s: child status=%d\n", label, status);
		exit(1);
	}
}

static void expect_select_readable(int fd, const char *label)
{
	fd_set rfds;
	struct timeval tv;
	int rc;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	rc = select(fd + 1, &rfds, NULL, NULL, &tv);
	if (rc < 0)
		die("select readable");
	if (rc != 1 || !FD_ISSET(fd, &rfds)) {
		fprintf(stderr, "%s: expected readable, rc=%d\n", label, rc);
		exit(1);
	}
}

static void expect_select_not_readable(int fd, const char *label)
{
	fd_set rfds;
	struct timeval tv;
	int rc;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	rc = select(fd + 1, &rfds, NULL, NULL, &tv);
	if (rc < 0)
		die("select !readable");
	if (rc != 0 || FD_ISSET(fd, &rfds)) {
		fprintf(stderr, "%s: expected not readable, rc=%d isset=%d\n",
			label, rc, FD_ISSET(fd, &rfds));
		exit(1);
	}
}

static void expect_select_writable(int fd, const char *label)
{
	fd_set wfds;
	struct timeval tv;
	int rc;

	FD_ZERO(&wfds);
	FD_SET(fd, &wfds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	rc = select(fd + 1, NULL, &wfds, NULL, &tv);
	if (rc < 0)
		die("select writable");
	if (rc != 1 || !FD_ISSET(fd, &wfds)) {
		fprintf(stderr, "%s: expected writable, rc=%d\n", label, rc);
		exit(1);
	}
}

static void expect_select_readable_blocking(int fd, int timeout_ms,
					    const char *label)
{
	fd_set rfds;
	struct timeval tv;
	int rc;
	int remaining = timeout_ms;

	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		tv.tv_sec = remaining / 1000;
		tv.tv_usec = (remaining % 1000) * 1000;
		rc = select(fd + 1, &rfds, NULL, NULL, &tv);
		if (rc >= 0)
			break;
		if (errno != EINTR)
			die("blocking select readable");
		remaining = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
	}
	if (rc != 1 || !FD_ISSET(fd, &rfds)) {
		fprintf(stderr, "%s: expected blocking readability, rc=%d\n",
			label, rc);
		exit(1);
	}
}

static void expect_poll_ready(int fd, short events, short want_bits,
			      short forbid_bits, const char *label)
{
	struct pollfd pfd;
	int rc;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = events;
	rc = poll(&pfd, 1, 0);
	if (rc < 0)
		die("poll ready");
	if (rc != 1 || (pfd.revents & want_bits) != want_bits ||
	    (pfd.revents & forbid_bits) != 0) {
		fprintf(stderr,
			"%s: expected poll rc=1 revents&%#x==%#x without %#x, got rc=%d revents=%#x\n",
			label, (unsigned short)want_bits, (unsigned short)want_bits,
			(unsigned short)forbid_bits, rc, (unsigned short)pfd.revents);
		exit(1);
	}
}

static void expect_poll_not_ready(int fd, short events, const char *label)
{
	struct pollfd pfd;
	int rc;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = events;
	rc = poll(&pfd, 1, 0);
	if (rc < 0)
		die("poll !ready");
	if (rc != 0 || pfd.revents != 0) {
		fprintf(stderr, "%s: expected timeout, rc=%d revents=%#x\n",
			label, rc, (unsigned short)pfd.revents);
		exit(1);
	}
}

static void expect_poll_ready_blocking(int fd, short events, int timeout_ms,
				       short want_bits, short forbid_bits,
				       const char *label)
{
	struct pollfd pfd;
	int rc;
	int remaining = timeout_ms;

	for (;;) {
		memset(&pfd, 0, sizeof(pfd));
		pfd.fd = fd;
		pfd.events = events;
		rc = poll(&pfd, 1, remaining);
		if (rc >= 0)
			break;
		if (errno != EINTR)
			die("blocking poll ready");
	}
	if (rc != 1 || (pfd.revents & want_bits) != want_bits ||
	    (pfd.revents & forbid_bits) != 0) {
		fprintf(stderr,
			"%s: expected blocking poll rc=1 revents&%#x==%#x without %#x, got rc=%d revents=%#x\n",
			label, (unsigned short)want_bits, (unsigned short)want_bits,
			(unsigned short)forbid_bits, rc, (unsigned short)pfd.revents);
		exit(1);
	}
}

static void expect_read_errno(int fd, int want1, int want2,
			      const char *label)
{
	char ch = 0;
	ssize_t n;

	errno = 0;
	n = read(fd, &ch, 1);
	if (n != -1) {
		fprintf(stderr, "%s: expected -1, got %ld\n", label, (long)n);
		exit(1);
	}
	expect_errno_is(errno, want1, want2, label);
}

static void expect_recv_errno(int fd, int want1, int want2,
			      const char *label)
{
	char ch = 0;
	ssize_t n;

	errno = 0;
	n = recv(fd, &ch, 1, 0);
	if (n != -1) {
		fprintf(stderr, "%s: expected -1, got %ld\n", label, (long)n);
		exit(1);
	}
	expect_errno_is(errno, want1, want2, label);
}

static void expect_read_exact(int fd, const void *want, size_t len,
			      const char *label)
{
	char buf[256];
	ssize_t n;

	if (len > sizeof(buf))
		failf(label, "buffer too small");
	memset(buf, 0, sizeof(buf));
	n = read(fd, buf, len);
	if (n != (ssize_t)len) {
		fprintf(stderr, "%s: expected read %lu, got %ld errno=%d\n",
			label, (unsigned long)len, (long)n, errno);
		exit(1);
	}
	if (memcmp(buf, want, len) != 0)
		failf(label, "payload mismatch");
}

static void expect_recv_exact(int fd, const void *want, size_t len,
			      const char *label)
{
	char buf[256];
	ssize_t n;

	if (len > sizeof(buf))
		failf(label, "buffer too small");
	memset(buf, 0, sizeof(buf));
	n = recv(fd, buf, len, 0);
	if (n != (ssize_t)len) {
		fprintf(stderr, "%s: expected recv %lu, got %ld errno=%d\n",
			label, (unsigned long)len, (long)n, errno);
		exit(1);
	}
	if (memcmp(buf, want, len) != 0)
		failf(label, "payload mismatch");
}

static void expect_send_exact(int fd, const void *buf, size_t len,
			      const char *label)
{
	ssize_t n = send(fd, buf, len, 0);

	if (n != (ssize_t)len) {
		fprintf(stderr, "%s: expected send %lu, got %ld errno=%d\n",
			label, (unsigned long)len, (long)n, errno);
		exit(1);
	}
}

static void expect_recv_zero(int fd, const char *label)
{
	char ch = 0;
	ssize_t n = recv(fd, &ch, 1, 0);

	if (n != 0) {
		fprintf(stderr, "%s: expected EOF/0, got %ld errno=%d\n", label,
			(long)n, errno);
		exit(1);
	}
}

static void expect_epipe_send(int fd, const char *label)
{
	ssize_t n;

	errno = 0;
	n = send(fd, "X", 1, 0);
	if (n != -1 || errno != EPIPE) {
		fprintf(stderr, "%s: expected -1/EPIPE, got n=%ld errno=%d\n",
			label, (long)n, errno);
		exit(1);
	}
}

static void expect_sockopt_int(int fd, int level, int optname, int want,
			       const char *label)
{
	int got = -1;
	socklen_t len = sizeof(got);

	if (getsockopt(fd, level, optname, &got, &len) < 0)
		die(label);
	if (len != sizeof(got) || got != want) {
		fprintf(stderr, "%s: expected %d, got %d len=%u\n", label, want,
			got, (unsigned)len);
		exit(1);
	}
}

static void expect_socktype(int fd, int want, const char *label)
{
	expect_sockopt_int(fd, SOL_SOCKET, SO_TYPE, want, label);
}

static void fill_unix_addr(struct sockaddr_un *addr, const char *path)
{
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	strncpy(addr->sun_path, path, sizeof(addr->sun_path) - 1);
}

static void expect_unix_name_path(int fd, int peer, const char *want_path,
				  const char *label)
{
	struct sockaddr_un addr;
	socklen_t len = sizeof(addr);
	int rc;

	memset(&addr, 0, sizeof(addr));
	rc = peer ? getpeername(fd, (struct sockaddr *)&addr, &len) :
		    getsockname(fd, (struct sockaddr *)&addr, &len);
	if (rc < 0)
		die(label);
	if (addr.sun_family != AF_UNIX) {
		fprintf(stderr, "%s: expected AF_UNIX, got %d\n", label,
			(int)addr.sun_family);
		exit(1);
	}
	if (strcmp(addr.sun_path, want_path) != 0) {
		fprintf(stderr, "%s: expected path '%s', got '%s'\n", label,
			want_path, addr.sun_path);
		exit(1);
	}
}

static void expect_inet_name(int fd, int peer, uint32_t want_addr,
			     unsigned short want_port, int require_port,
			     const char *label)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int rc;

	memset(&addr, 0, sizeof(addr));
	rc = peer ? getpeername(fd, (struct sockaddr *)&addr, &len) :
		    getsockname(fd, (struct sockaddr *)&addr, &len);
	if (rc < 0)
		die(label);
	if (addr.sin_family != AF_INET) {
		fprintf(stderr, "%s: expected AF_INET, got %d\n", label,
			(int)addr.sin_family);
		exit(1);
	}
	if (addr.sin_addr.s_addr != want_addr) {
		fprintf(stderr, "%s: expected addr %#x, got %#x\n", label,
			(unsigned)ntohl(want_addr), (unsigned)ntohl(addr.sin_addr.s_addr));
		exit(1);
	}
	if (require_port && ntohs(addr.sin_port) != want_port) {
		fprintf(stderr, "%s: expected port %u, got %u\n", label,
			(unsigned)want_port, (unsigned)ntohs(addr.sin_port));
		exit(1);
	}
	if (!require_port && addr.sin_port == 0)
		failf(label, "expected nonzero port");
}

static void fill_loopback_addr(struct sockaddr_in *addr, unsigned short port)
{
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr->sin_port = htons(port);
}

static unsigned short bound_port(int fd, const char *label)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);

	memset(&addr, 0, sizeof(addr));
	if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0)
		die(label);
	if (addr.sin_family != AF_INET || addr.sin_port == 0)
		failf(label, "expected bound AF_INET port");
	return ntohs(addr.sin_port);
}

static void test_unix_socketpair_stream(void)
{
	int sv[2];
	struct iovec iov[2];
	struct msghdr msg;
	char rbuf[8];
	char p1 = 'A';
	char p2 = 'B';

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		die("socketpair");

	expect_socktype(sv[0], SOCK_STREAM, "socketpair SO_TYPE end0");
	expect_socktype(sv[1], SOCK_STREAM, "socketpair SO_TYPE end1");
	expect_unix_name_path(sv[0], 0, "", "socketpair getsockname end0");
	expect_unix_name_path(sv[0], 1, "", "socketpair getpeername end0");
	expect_select_not_readable(sv[0], "socketpair initially not readable");
	expect_select_writable(sv[0], "socketpair end0 initially writable");
	expect_select_writable(sv[1], "socketpair end1 initially writable");
	expect_poll_not_ready(sv[0], POLLIN, "socketpair initially poll timeout");
	expect_poll_ready(sv[0], POLLOUT, POLLOUT, 0,
			  "socketpair poll writable");

	set_nonblock(sv[0], 1, "fcntl socketpair end0 nonblock");
	expect_recv_errno(sv[0], EAGAIN, EWOULDBLOCK,
			  "socketpair nonblocking recv empty");

	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = &p1;
	iov[0].iov_len = 1;
	iov[1].iov_base = &p2;
	iov[1].iov_len = 1;
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	if (sendmsg(sv[1], &msg, 0) != 2)
		die("socketpair sendmsg");

	expect_select_readable(sv[0], "socketpair readable after sendmsg");
	expect_poll_ready(sv[0], POLLIN, POLLIN, POLLHUP,
			  "socketpair poll readable after sendmsg");

	memset(&msg, 0, sizeof(msg));
	memset(rbuf, 0, sizeof(rbuf));
	iov[0].iov_base = rbuf;
	iov[0].iov_len = 1;
	iov[1].iov_base = rbuf + 1;
	iov[1].iov_len = 1;
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	if (recvmsg(sv[0], &msg, 0) != 2)
		die("socketpair recvmsg");
	if (memcmp(rbuf, "AB", 2) != 0)
		failf("socketpair recvmsg", "payload mismatch");

	if (shutdown(sv[1], SHUT_RDWR) < 0)
		die("socketpair shutdown");
	expect_select_readable(sv[0], "socketpair EOF is select-readable");
	expect_poll_ready(sv[0], POLLIN, POLLIN | POLLHUP, 0,
			  "socketpair EOF is poll read+hup");
	expect_recv_zero(sv[0], "socketpair EOF recv");
	expect_epipe_send(sv[0], "socketpair send after peer shutdown");

	close(sv[0]);
	close(sv[1]);
}

static void test_named_unix_stream(void)
{
	char path[256];
	struct sockaddr_un addr;
	struct sockaddr_un peer_addr;
	socklen_t peer_len;
	int listenfd, clientfd, connfd;
	int pipefd[2];
	pid_t pid;

	snprintf(path, sizeof(path), "%s/named-stream.sock",
		 "/root/tests/posix_socket");
	unlink(path);
	fill_unix_addr(&addr, path);

	listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listenfd < 0)
		die("unix stream listen socket");
	expect_socktype(listenfd, SOCK_STREAM, "unix listener SO_TYPE");
	expect_unix_name_path(listenfd, 0, "", "unix listener getsockname before bind");

	if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("unix stream bind");
	expect_unix_name_path(listenfd, 0, path, "unix listener getsockname after bind");
	if (listen(listenfd, 2) < 0)
		die("unix stream listen");

	expect_select_not_readable(listenfd, "unix listener initially not readable");
	expect_poll_not_ready(listenfd, POLLIN, "unix listener initially no pending accept");

	if (pipe(pipefd) < 0)
		die("unix stream pipe");
	pid = fork();
	if (pid < 0)
		die("fork unix stream");
	if (pid == 0) {
		close(pipefd[0]);
		clientfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (clientfd < 0)
			_exit(1);
		if (connect(clientfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
			_exit(2);
		if (write(pipefd[1], "R", 1) != 1)
			_exit(3);
		usleep(100000);
		if (send(clientfd, "U", 1, 0) != 1)
			_exit(4);
		usleep(100000);
		if (shutdown(clientfd, SHUT_RDWR) < 0)
			_exit(5);
		close(clientfd);
		close(pipefd[1]);
		_exit(0);
	}

	close(pipefd[1]);
	expect_select_readable_blocking(
		listenfd, 2000,
		"blocking select wakes for named unix pending accept");

	peer_len = sizeof(peer_addr);
	memset(&peer_addr, 0, sizeof(peer_addr));
	connfd = accept(listenfd, (struct sockaddr *)&peer_addr, &peer_len);
	if (connfd < 0)
		die("unix stream accept");
	if (peer_addr.sun_family != AF_UNIX || strcmp(peer_addr.sun_path, "") != 0)
		failf("unix stream accept peer", "unexpected accept peer path");
	expect_unix_name_path(connfd, 0, path, "unix accepted getsockname");
	expect_unix_name_path(connfd, 1, "", "unix accepted getpeername");

	{
		char ready = 0;
		if (read(pipefd[0], &ready, 1) != 1 || ready != 'R')
			die("unix stream ready pipe");
	}
	close(pipefd[0]);

	set_nonblock(connfd, 1, "fcntl unix conn nonblock");
	expect_read_errno(connfd, EAGAIN, EWOULDBLOCK,
			  "unix accepted nonblocking read while idle");
	expect_select_writable(connfd, "unix accepted initially writable");

	expect_poll_ready_blocking(
		connfd, POLLIN, 2000, POLLIN, POLLHUP,
		"blocking poll wakes for unix accepted payload");
	expect_read_exact(connfd, "U", 1, "unix accepted payload");

	expect_poll_ready_blocking(
		connfd, POLLIN, 2000, POLLIN | POLLHUP, 0,
		"blocking poll wakes for unix accepted EOF");
	expect_recv_zero(connfd, "unix accepted EOF recv");
	expect_epipe_send(connfd, "unix accepted send after peer shutdown");

	close(connfd);
	close(listenfd);
	wait_child_ok(pid, "named unix stream");
	unlink(path);
}

static void test_unix_dgram(void)
{
	char srv_path[256];
	char cli_path[256];
	struct sockaddr_un srv_addr;
	struct sockaddr_un cli_addr;
	struct sockaddr_un from;
	socklen_t fromlen;
	int srvfd, clifd;
	struct iovec iov[2];
	struct msghdr msg;
	char rbuf[16];

	snprintf(srv_path, sizeof(srv_path), "%s/unix-dgram-srv.sock",
		 "/root/tests/posix_socket");
	snprintf(cli_path, sizeof(cli_path), "%s/unix-dgram-cli.sock",
		 "/root/tests/posix_socket");
	unlink(srv_path);
	unlink(cli_path);
	fill_unix_addr(&srv_addr, srv_path);
	fill_unix_addr(&cli_addr, cli_path);

	srvfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (srvfd < 0)
		die("unix dgram server socket");
	clifd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (clifd < 0)
		die("unix dgram client socket");

	expect_socktype(srvfd, SOCK_DGRAM, "unix dgram server SO_TYPE");
	expect_socktype(clifd, SOCK_DGRAM, "unix dgram client SO_TYPE");
	if (bind(srvfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0)
		die("unix dgram bind server");
	if (bind(clifd, (struct sockaddr *)&cli_addr, sizeof(cli_addr)) < 0)
		die("unix dgram bind client");
	expect_unix_name_path(srvfd, 0, srv_path, "unix dgram server getsockname");
	expect_unix_name_path(clifd, 0, cli_path, "unix dgram client getsockname");

	set_nonblock(srvfd, 1, "fcntl unix dgram srv nonblock");
	expect_recv_errno(srvfd, EAGAIN, EWOULDBLOCK,
			  "unix dgram nonblocking recv empty");
	expect_poll_not_ready(srvfd, POLLIN, "unix dgram server initially no data");

	if (connect(srvfd, (struct sockaddr *)&cli_addr, sizeof(cli_addr)) < 0)
		die("unix dgram connect server");
	if (connect(clifd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0)
		die("unix dgram connect client");
	expect_unix_name_path(srvfd, 1, cli_path, "unix dgram server getpeername");
	expect_unix_name_path(clifd, 1, srv_path, "unix dgram client getpeername");

	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = "hi";
	iov[0].iov_len = 2;
	iov[1].iov_base = "!";
	iov[1].iov_len = 1;
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	if (sendmsg(clifd, &msg, 0) != 3)
		die("unix dgram sendmsg");

	expect_select_readable(srvfd, "unix dgram readable after sendmsg");
	memset(&msg, 0, sizeof(msg));
	memset(&from, 0, sizeof(from));
	memset(rbuf, 0, sizeof(rbuf));
	fromlen = sizeof(from);
	iov[0].iov_base = rbuf;
	iov[0].iov_len = sizeof(rbuf);
	msg.msg_name = &from;
	msg.msg_namelen = fromlen;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	if (recvmsg(srvfd, &msg, 0) != 3)
		die("unix dgram recvmsg");
	if (memcmp(rbuf, "hi!", 3) != 0)
		failf("unix dgram recvmsg", "payload mismatch");
	if (from.sun_family != AF_UNIX || strcmp(from.sun_path, cli_path) != 0)
		failf("unix dgram recvmsg", "unexpected source path");

	expect_send_exact(srvfd, "OK", 2, "unix dgram connected send");
	expect_select_readable(clifd, "unix dgram client readable after connected send");
	expect_recv_exact(clifd, "OK", 2, "unix dgram connected recv");

	close(srvfd);
	close(clifd);
	unlink(srv_path);
	unlink(cli_path);
}

static void test_loopback_tcp(void)
{
	struct sockaddr_in addr;
	struct sockaddr_in peer_addr;
	socklen_t peer_len;
	int listenfd, clientfd, connfd;
	int yes = 1;
	unsigned short port;
	int pipefd[2];
	pid_t pid;
	char buf[8];
	struct iovec iov[2];
	struct msghdr msg;

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd < 0)
		die("tcp listen socket");
	expect_socktype(listenfd, SOCK_STREAM, "tcp listener SO_TYPE");
	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
		die("tcp setsockopt SO_REUSEADDR");
	expect_sockopt_int(listenfd, SOL_SOCKET, SO_REUSEADDR, 1,
			   "tcp getsockopt SO_REUSEADDR");

	fill_loopback_addr(&addr, 0);
	if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("tcp bind");
	port = bound_port(listenfd, "tcp bound port");
	expect_inet_name(listenfd, 0, htonl(INADDR_LOOPBACK), port, 1,
			 "tcp listener getsockname after bind");
	if (listen(listenfd, 2) < 0)
		die("tcp listen");

	expect_select_not_readable(listenfd, "tcp listener initially not readable");
	expect_poll_not_ready(listenfd, POLLIN, "tcp listener initially no pending accept");

	if (pipe(pipefd) < 0)
		die("tcp pipe");
	pid = fork();
	if (pid < 0)
		die("fork tcp");
	if (pid == 0) {
		close(pipefd[0]);
		clientfd = socket(AF_INET, SOCK_STREAM, 0);
		if (clientfd < 0)
			_exit(1);
		if (setsockopt(clientfd, SOL_SOCKET, SO_KEEPALIVE, &yes,
			       sizeof(yes)) < 0)
			_exit(2);
		if (setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, &yes,
			       sizeof(yes)) < 0)
			_exit(3);
		fill_loopback_addr(&addr, port);
		if (connect(clientfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
			_exit(4);
		if (write(pipefd[1], "C", 1) != 1)
			_exit(5);
		usleep(100000);
		if (send(clientfd, "L", 1, 0) != 1)
			_exit(6);
		{
			char reply[4];
			ssize_t n = recv(clientfd, reply, sizeof(reply), 0);
			if (n != 4 || memcmp(reply, "PING", 4) != 0)
				_exit(8);
		}
		usleep(100000);
		if (shutdown(clientfd, SHUT_RDWR) < 0)
			_exit(7);
		close(clientfd);
		close(pipefd[1]);
		_exit(0);
	}

	close(pipefd[1]);
	expect_select_readable_blocking(
		listenfd, 2000,
		"blocking select wakes for tcp pending accept");
	peer_len = sizeof(peer_addr);
	memset(&peer_addr, 0, sizeof(peer_addr));
	connfd = accept(listenfd, (struct sockaddr *)&peer_addr, &peer_len);
	if (connfd < 0)
		die("tcp accept");
	if (peer_addr.sin_family != AF_INET)
		failf("tcp accept peer", "expected AF_INET");
	expect_inet_name(connfd, 0, htonl(INADDR_LOOPBACK), port, 1,
			 "tcp accepted getsockname");
	expect_inet_name(connfd, 1, htonl(INADDR_LOOPBACK), ntohs(peer_addr.sin_port), 1,
			 "tcp accepted getpeername");

	{
		char ready = 0;
		if (read(pipefd[0], &ready, 1) != 1 || ready != 'C')
			die("tcp ready");
	}
	close(pipefd[0]);

	expect_sockopt_int(connfd, SOL_SOCKET, SO_TYPE, SOCK_STREAM,
			   "tcp accepted SO_TYPE");
	set_nonblock(connfd, 1, "fcntl tcp accepted nonblock");
	expect_recv_errno(connfd, EAGAIN, EWOULDBLOCK,
			  "tcp accepted nonblocking recv while idle");
	expect_select_writable(connfd, "tcp accepted initially writable");

	expect_poll_ready_blocking(
		connfd, POLLIN, 2000, POLLIN, POLLHUP,
		"blocking poll wakes for tcp payload");
	expect_recv_exact(connfd, "L", 1, "tcp recv payload");

	memset(&msg, 0, sizeof(msg));
	memset(buf, 0, sizeof(buf));
	iov[0].iov_base = buf;
	iov[0].iov_len = 2;
	iov[1].iov_base = buf + 2;
	iov[1].iov_len = 2;
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	expect_send_exact(connfd, "PING", 4, "tcp send to client after read");
	if (recvmsg(connfd, &msg, MSG_DONTWAIT) != -1 || errno != EAGAIN)
		failf("tcp recvmsg dontwait", "expected EAGAIN on empty stream");

	expect_poll_ready_blocking(
		connfd, POLLIN, 2000, POLLIN | POLLHUP, 0,
		"blocking poll wakes for tcp EOF");
	expect_recv_zero(connfd, "tcp EOF recv");
	expect_send_exact(connfd, "Z", 1,
			  "tcp send may still succeed after peer EOF");

	close(connfd);
	close(listenfd);
	wait_child_ok(pid, "loopback tcp");
}

static void test_loopback_udp(void)
{
	struct sockaddr_in srv_addr;
	struct sockaddr_in cli_addr;
	struct sockaddr_in from_addr;
	socklen_t from_len;
	int srvfd, clifd;
	int yes = 1;
	unsigned short srv_port;
	unsigned short cli_port;
	char buf[16];
	struct iovec iov[2];
	struct msghdr msg;

	srvfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (srvfd < 0)
		die("udp server socket");
	clifd = socket(AF_INET, SOCK_DGRAM, 0);
	if (clifd < 0)
		die("udp client socket");

	expect_socktype(srvfd, SOCK_DGRAM, "udp server SO_TYPE");
	expect_socktype(clifd, SOCK_DGRAM, "udp client SO_TYPE");
	if (setsockopt(srvfd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0)
		die("udp setsockopt SO_BROADCAST");
	expect_sockopt_int(srvfd, SOL_SOCKET, SO_BROADCAST, 1,
			   "udp getsockopt SO_BROADCAST");

	fill_loopback_addr(&srv_addr, 0);
	fill_loopback_addr(&cli_addr, 0);
	if (bind(srvfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0)
		die("udp bind server");
	if (bind(clifd, (struct sockaddr *)&cli_addr, sizeof(cli_addr)) < 0)
		die("udp bind client");
	srv_port = bound_port(srvfd, "udp server bound port");
	cli_port = bound_port(clifd, "udp client bound port");
	expect_inet_name(srvfd, 0, htonl(INADDR_LOOPBACK), srv_port, 1,
			 "udp server getsockname");
	expect_inet_name(clifd, 0, htonl(INADDR_LOOPBACK), cli_port, 1,
			 "udp client getsockname");

	set_nonblock(srvfd, 1, "fcntl udp srv nonblock");
	expect_recv_errno(srvfd, EAGAIN, EWOULDBLOCK,
			  "udp nonblocking recv empty");
	expect_poll_not_ready(srvfd, POLLIN, "udp server initially not readable");

	fill_loopback_addr(&srv_addr, srv_port);
	if (sendto(clifd, "UDP", 3, 0, (struct sockaddr *)&srv_addr,
		   sizeof(srv_addr)) != 3)
		die("udp sendto");

	expect_select_readable_blocking(
		srvfd, 2000, "udp server readable after sendto");
	memset(buf, 0, sizeof(buf));
	memset(&from_addr, 0, sizeof(from_addr));
	from_len = sizeof(from_addr);
	if (recvfrom(srvfd, buf, sizeof(buf), 0, (struct sockaddr *)&from_addr,
		     &from_len) != 3)
		die("udp recvfrom");
	if (memcmp(buf, "UDP", 3) != 0)
		failf("udp recvfrom", "payload mismatch");
	if (from_addr.sin_family != AF_INET ||
	    from_addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
	    ntohs(from_addr.sin_port) != cli_port) {
		failf("udp recvfrom", "unexpected source address");
	}

	fill_loopback_addr(&cli_addr, cli_port);
	if (connect(srvfd, (struct sockaddr *)&cli_addr, sizeof(cli_addr)) < 0)
		die("udp connect server");
	if (connect(clifd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0)
		die("udp connect client");
	expect_inet_name(srvfd, 1, htonl(INADDR_LOOPBACK), cli_port, 1,
			 "udp server getpeername");
	expect_inet_name(clifd, 1, htonl(INADDR_LOOPBACK), srv_port, 1,
			 "udp client getpeername");

	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = "re";
	iov[0].iov_len = 2;
	iov[1].iov_base = "ply";
	iov[1].iov_len = 3;
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	if (sendmsg(srvfd, &msg, 0) != 5)
		die("udp sendmsg");

	expect_poll_ready_blocking(
		clifd, POLLIN, 2000, POLLIN, POLLHUP,
		"udp client poll readable after sendmsg");
	memset(&msg, 0, sizeof(msg));
	memset(buf, 0, sizeof(buf));
	memset(&from_addr, 0, sizeof(from_addr));
	from_len = sizeof(from_addr);
	iov[0].iov_base = buf;
	iov[0].iov_len = sizeof(buf);
	msg.msg_name = &from_addr;
	msg.msg_namelen = from_len;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	if (recvmsg(clifd, &msg, 0) != 5)
		die("udp recvmsg");
	if (memcmp(buf, "reply", 5) != 0)
		failf("udp recvmsg", "payload mismatch");
	if (from_addr.sin_family != AF_INET ||
	    from_addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
	    ntohs(from_addr.sin_port) != srv_port) {
		failf("udp recvmsg", "unexpected source address");
	}

	close(srvfd);
	close(clifd);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	test_unix_socketpair_stream();
	test_named_unix_stream();
	test_unix_dgram();
	test_loopback_tcp();
	test_loopback_udp();
	return 0;
}
EOF

expect_success gcc -Wall -Werror -o "$BIN" "$SRC"
update_case_timestamp
fi
expect_success "$BIN"
