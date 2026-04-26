#!/bin/sh

set -e
CASE_NAME=posix_poll_matrix
CASE_MTIME=@CASE_MTIME@
BASE=/root/tests/$CASE_NAME
WORKDIR=/root/tests/$CASE_NAME
SRC="$BASE/poll_select_matrix.c"
BIN="$BASE/poll_select_matrix"
STAMP="$BASE/.case_timestamp"

fail()
{
	echo "posix_poll_select_matrix: $1" >&2
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

expect_success_sh()
{
	set +e
	output=$(sh -c "$1" 2>&1)
	rc=$?
	set -e
	[ "$rc" -eq 0 ] && return 0
	fail "Expected: success from '$1'
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
require_cmd grep

expect_success_sh "grep -q '^devpts /dev/pts ' /proc/mounts"

if prepare_embedded_c; then
cat > "$SRC" <<'EOF'
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#ifndef TIOCGPTN
#define TIOCGPTN 0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static void set_nonblock(int fd, const char *label)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0)
		die(label);
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		die(label);
}

static void expect_eagain_read(int fd, const char *label)
{
	char ch = 0;
	ssize_t n;

	errno = 0;
	n = read(fd, &ch, 1);
	if (n != -1 || errno != EAGAIN) {
		fprintf(stderr, "%s: expected -1/EAGAIN, got n=%ld errno=%d\n",
			label, (long)n, errno);
		exit(1);
	}
}

static void expect_eof_read(int fd, const char *label)
{
	char ch = 0;
	ssize_t n;

	n = read(fd, &ch, 1);
	if (n != 0) {
		fprintf(stderr, "%s: expected EOF, got n=%ld errno=%d\n", label,
			(long)n, errno);
		exit(1);
	}
}

static void expect_epipe_write(int fd, const char *label)
{
	ssize_t n;

	errno = 0;
	n = write(fd, "X", 1);
	if (n != -1 || errno != EPIPE) {
		fprintf(stderr, "%s: expected -1/EPIPE, got n=%ld errno=%d\n",
			label, (long)n, errno);
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
	if (rc == 0 || !FD_ISSET(fd, &rfds)) {
		fprintf(stderr, "%s: expected select readability, rc=%d\n",
			label, rc);
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
		fprintf(stderr,
			"%s: expected select to report not-readable, rc=%d isset=%d\n",
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
	if (rc == 0 || !FD_ISSET(fd, &wfds)) {
		fprintf(stderr, "%s: expected select writability, rc=%d\n",
			label, rc);
		exit(1);
	}
}

static void expect_select_not_writable(int fd, const char *label)
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
		die("select !writable");
	if (rc != 0 || FD_ISSET(fd, &wfds)) {
		fprintf(stderr,
			"%s: expected select to report not-writable, rc=%d isset=%d\n",
			label, rc, FD_ISSET(fd, &wfds));
		exit(1);
	}
}

static void expect_select_not_excepted(int fd, const char *label)
{
	fd_set efds;
	struct timeval tv;
	int rc;

	FD_ZERO(&efds);
	FD_SET(fd, &efds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	rc = select(fd + 1, NULL, NULL, &efds, &tv);
	if (rc < 0)
		die("select !except");
	if (rc != 0 || FD_ISSET(fd, &efds)) {
		fprintf(stderr,
			"%s: expected select to report no exception, rc=%d isset=%d\n",
			label, rc, FD_ISSET(fd, &efds));
		exit(1);
	}
}

static void expect_select_ebadf(int fd, int mode, const char *label)
{
	fd_set rfds;
	fd_set wfds;
	fd_set efds;
	struct timeval tv;
	int rc;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	if (mode == 0)
		FD_SET(fd, &rfds);
	else if (mode == 1)
		FD_SET(fd, &wfds);
	else
		FD_SET(fd, &efds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	errno = 0;
	rc = select(fd + 1, mode == 0 ? &rfds : NULL, mode == 1 ? &wfds : NULL,
		    mode == 2 ? &efds : NULL, &tv);
	if (rc != -1 || errno != EBADF) {
		fprintf(stderr, "%s: expected -1/EBADF, got rc=%d errno=%d\n",
			label, rc, errno);
		exit(1);
	}
}

static void expect_select_zero_timeout(void)
{
	struct timeval tv;
	int rc;

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	rc = select(0, NULL, NULL, NULL, &tv);
	if (rc != 0) {
		fprintf(stderr, "select(0, NULL, NULL, NULL, {0,0}) rc=%d\n",
			rc);
		exit(1);
	}
}

static void expect_select_readable_blocking(int fd, int timeout_ms,
					    const char *label)
{
	fd_set rfds;
	struct timeval tv;
	int rc;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	rc = select(fd + 1, &rfds, NULL, NULL, &tv);
	if (rc < 0)
		die("select readable blocking");
	if (rc != 1 || !FD_ISSET(fd, &rfds)) {
		fprintf(stderr,
			"%s: expected blocking select readability, rc=%d isset=%d\n",
			label, rc, FD_ISSET(fd, &rfds));
		exit(1);
	}
}

static void expect_poll_ready(int fd, short events, short must_have,
			      short must_not_have, const char *label)
{
	struct pollfd pfd;
	int rc;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = events;
	rc = poll(&pfd, 1, 0);
	if (rc < 0)
		die("poll");
	if (rc == 0) {
		fprintf(stderr, "%s: expected poll readiness, rc=0\n", label);
		exit(1);
	}
	if ((pfd.revents & must_have) != must_have) {
		fprintf(stderr,
			"%s: missing poll bits, revents=%#x expected=%#x\n",
			label, pfd.revents, must_have);
		exit(1);
	}
	if (pfd.revents & must_not_have) {
		fprintf(stderr,
			"%s: unexpected poll bits, revents=%#x forbidden=%#x\n",
			label, pfd.revents, must_not_have);
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
		fprintf(stderr,
			"%s: expected poll timeout, rc=%d revents=%#x\n",
			label, rc, pfd.revents);
		exit(1);
	}
}

static void expect_poll_exact(int fd, short events, short exact_revents,
			      const char *label)
{
	struct pollfd pfd;
	int rc;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = events;
	rc = poll(&pfd, 1, 0);
	if (rc < 0)
		die("poll exact");
	if (rc != (exact_revents ? 1 : 0) || pfd.revents != exact_revents) {
		fprintf(stderr,
			"%s: expected rc=%d revents=%#x, got rc=%d revents=%#x\n",
			label, exact_revents ? 1 : 0, exact_revents, rc,
			pfd.revents);
		exit(1);
	}
}

static void expect_poll_nfds_zero(void)
{
	int rc = poll(NULL, 0, 0);

	if (rc != 0) {
		fprintf(stderr, "poll(NULL, 0, 0) rc=%d\n", rc);
		exit(1);
	}
}

static void expect_poll_ignored_negative_fd(short events, const char *label)
{
	struct pollfd pfd;
	int rc;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = -1;
	pfd.events = events;
	rc = poll(&pfd, 1, 0);
	if (rc != 0 || pfd.revents != 0) {
		fprintf(stderr,
			"%s: expected rc=0 revents=0, got rc=%d revents=%#x\n",
			label, rc, pfd.revents);
		exit(1);
	}
}

static void expect_poll_ready_blocking(int fd, short events, int timeout_ms,
				       short must_have, short must_not_have,
				       const char *label)
{
	struct pollfd pfd;
	int rc;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = events;
	rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0)
		die("poll blocking");
	if (rc != 1) {
		fprintf(stderr, "%s: expected blocking poll rc=1, got rc=%d\n",
			label, rc);
		exit(1);
	}
	if ((pfd.revents & must_have) != must_have) {
		fprintf(stderr,
			"%s: missing blocking poll bits, revents=%#x expected=%#x\n",
			label, pfd.revents, must_have);
		exit(1);
	}
	if (pfd.revents & must_not_have) {
		fprintf(stderr,
			"%s: unexpected blocking poll bits, revents=%#x forbidden=%#x\n",
			label, pfd.revents, must_not_have);
		exit(1);
	}
}

static long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static void expect_select_readable_blocking_prompt(int fd, int timeout_ms,
						   int prompt_ms,
						   const char *label)
{
	fd_set rfds;
	struct timeval tv;
	long t0;
	long elapsed;
	int rc;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	t0 = now_ms();
	rc = select(fd + 1, &rfds, NULL, NULL, &tv);
	elapsed = now_ms() - t0;
	if (rc < 0)
		die("select readable blocking prompt");
	if (rc != 1 || !FD_ISSET(fd, &rfds)) {
		fprintf(stderr,
			"%s: expected blocking select readability, rc=%d isset=%d\n",
			label, rc, FD_ISSET(fd, &rfds));
		exit(1);
	}
	if (elapsed > prompt_ms) {
		fprintf(stderr,
			"%s: select woke too late, elapsed=%ldms limit=%dms\n",
			label, elapsed, prompt_ms);
		exit(1);
	}
}

static void expect_poll_ready_blocking_prompt(int fd, short events,
					      int timeout_ms, int prompt_ms,
					      short must_have,
					      short must_not_have,
					      const char *label)
{
	struct pollfd pfd;
	long t0;
	long elapsed;
	int rc;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = fd;
	pfd.events = events;
	t0 = now_ms();
	rc = poll(&pfd, 1, timeout_ms);
	elapsed = now_ms() - t0;
	if (rc < 0)
		die("poll blocking prompt");
	if (rc != 1) {
		fprintf(stderr,
			"%s: expected blocking poll rc=1, got rc=%d\n",
			label, rc);
		exit(1);
	}
	if ((pfd.revents & must_have) != must_have) {
		fprintf(stderr,
			"%s: missing blocking poll bits, revents=%#x expected=%#x\n",
			label, pfd.revents, must_have);
		exit(1);
	}
	if (pfd.revents & must_not_have) {
		fprintf(stderr,
			"%s: unexpected blocking poll bits, revents=%#x forbidden=%#x\n",
			label, pfd.revents, must_not_have);
		exit(1);
	}
	if (elapsed > prompt_ms) {
		fprintf(stderr,
			"%s: poll woke too late, elapsed=%ldms limit=%dms\n",
			label, elapsed, prompt_ms);
		exit(1);
	}
}

static void set_raw_mode(int fd)
{
	struct termios tio;

	if (ioctl(fd, TCGETS, &tio) != 0)
		die("TCGETS");
	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	if (ioctl(fd, TCSETS, &tio) != 0)
		die("TCSETS");
}

static void open_pts_pair(int *master, int *slave)
{
	char path[64];
	int idx;
	int lock = 0;

	*master = open("/dev/ptmx", O_RDWR);
	if (*master < 0)
		die("open /dev/ptmx");
	if (ioctl(*master, TIOCGPTN, &idx) != 0)
		die("TIOCGPTN");
	if (ioctl(*master, TIOCSPTLCK, &lock) != 0)
		die("TIOCSPTLCK");
	snprintf(path, sizeof(path), "/dev/pts/%d", idx);
	*slave = open(path, O_RDWR);
	if (*slave < 0)
		die("open slave pty");
}

static void wait_child_ok(pid_t pid, const char *label)
{
	int status;

	if (waitpid(pid, &status, 0) < 0)
		die("waitpid");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s: child failed with status=%d\n", label,
			status);
		exit(1);
	}
}

static void test_zero_fd_calls(void)
{
	expect_select_zero_timeout();
	expect_poll_nfds_zero();
	expect_poll_ignored_negative_fd(POLLIN | POLLOUT,
					"poll ignores negative fd");
}

static void test_invalid_fd(void)
{
	int fd;
	char path[256];

	snprintf(path, sizeof(path), "%s/invalid.txt",
		 "/root/tests/posix_poll_matrix");
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0)
		die("open invalid-fd file");
	if (close(fd) < 0)
		die("close invalid-fd file");

	expect_poll_exact(fd, POLLIN, POLLNVAL,
			  "poll closed fd read request returns POLLNVAL");
	expect_poll_exact(fd, POLLOUT, POLLNVAL,
			  "poll closed fd write request returns POLLNVAL");
	expect_poll_exact(fd, 0, POLLNVAL,
			  "poll closed fd no-event request returns POLLNVAL");

	expect_select_ebadf(fd, 0, "select closed fd in readfds returns EBADF");
	expect_select_ebadf(fd, 1, "select closed fd in writefds returns EBADF");
	expect_select_ebadf(fd, 2, "select closed fd in exceptfds returns EBADF");
}

static void test_ext4_file(void)
{
	char path[256];
	int fd;
	char ch = 0;

	snprintf(path, sizeof(path), "%s/file.txt", "/root/tests/posix_poll_matrix");
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0)
		die("open ext4 file");
	if (write(fd, "abc", 3) != 3)
		die("write ext4 file");
	if (lseek(fd, 0, SEEK_SET) < 0)
		die("lseek ext4 file");

	expect_select_readable(fd, "ext4 regular file select read-ready");
	expect_select_writable(fd, "ext4 regular file select write-ready");
	expect_select_not_excepted(fd, "ext4 regular file has no select except");
	expect_poll_ready(fd, POLLIN | POLLOUT, POLLIN | POLLOUT, 0,
			  "ext4 regular file poll read+write ready");
	expect_poll_not_ready(fd, POLLERR | POLLHUP | POLLNVAL,
			      "ext4 regular file has no poll error bits");

	if (lseek(fd, 3, SEEK_SET) < 0)
		die("lseek ext4 EOF");
	expect_select_readable(fd, "ext4 regular file stays read-ready at EOF");
	expect_select_writable(fd, "ext4 regular file stays write-ready at EOF");
	expect_select_not_excepted(fd,
				   "ext4 regular file stays non-excepted at EOF");
	expect_poll_ready(fd, POLLIN | POLLOUT, POLLIN | POLLOUT, 0,
			  "ext4 regular file poll stays ready at EOF");
	expect_poll_not_ready(fd, POLLERR | POLLHUP | POLLNVAL,
			      "ext4 regular file stays free of poll error bits at EOF");

	if (read(fd, &ch, 1) != 0) {
		fprintf(stderr, "ext4 regular file EOF read mismatch\n");
		exit(1);
	}
	if (close(fd) < 0)
		die("close ext4 file");
}

static void test_pipe(void)
{
	int p[2];
	char ch = 0;

	if (pipe(p) < 0)
		die("pipe");

	expect_select_not_readable(p[0], "pipe read end initially not readable");
	expect_select_not_writable(p[0], "pipe read end initially not writable");
	expect_select_not_excepted(p[0], "pipe read end initially no except");
	expect_select_writable(p[1], "pipe write end initially writable");
	expect_select_not_readable(p[1], "pipe write end initially not readable");
	expect_select_not_excepted(p[1], "pipe write end initially no except");
	expect_poll_not_ready(p[0], POLLIN, "pipe read end initially poll timeout");
	expect_poll_not_ready(p[0], POLLOUT,
			      "pipe read end initially not poll writable");
	expect_poll_ready(p[1], POLLOUT, POLLOUT, 0,
			  "pipe write end initially poll writable");
	expect_poll_not_ready(p[1], POLLIN,
			      "pipe write end initially not poll readable");

	if (write(p[1], "Q", 1) != 1)
		die("pipe write");
	expect_select_readable(p[0], "pipe read end readable after write");
	expect_select_not_writable(p[0], "pipe read end still not writable");
	expect_select_not_excepted(p[0], "pipe read end still no except after write");
	expect_poll_ready(p[0], POLLIN, POLLIN, POLLHUP,
			  "pipe read end poll readable after write");

	if (read(p[0], &ch, 1) != 1 || ch != 'Q') {
		fprintf(stderr, "pipe payload mismatch\n");
		exit(1);
	}
	expect_select_not_readable(p[0], "pipe drained read end not readable");

	if (close(p[1]) < 0)
		die("close pipe writer");
	expect_select_readable(p[0], "pipe EOF is select-readable");
	expect_select_not_excepted(p[0], "pipe EOF is not select-except");
	expect_poll_ready(p[0], POLLIN, POLLHUP, POLLIN,
			  "pipe EOF is poll hup-only");
	expect_poll_exact(p[0], 0, POLLHUP,
			  "pipe EOF returns POLLHUP even with no events requested");

	if (read(p[0], &ch, 1) != 0) {
		fprintf(stderr, "pipe EOF read mismatch\n");
		exit(1);
	}
	if (close(p[0]) < 0)
		die("close pipe reader");

	if (pipe(p) < 0)
		die("pipe writer error");
	if (close(p[0]) < 0)
		die("close pipe reader for writer error");
	expect_select_not_readable(p[1],
				   "pipe write end with no reader not readable");
	expect_select_not_excepted(p[1],
				   "pipe write end with no reader not excepted");
	expect_poll_ready(p[1], POLLOUT, POLLERR, POLLOUT | POLLHUP,
			  "pipe write end with no reader reports POLLERR");
	if (close(p[1]) < 0)
		die("close pipe writer error case");
}

static void test_pipe_nonblocking_io(void)
{
	int p[2];

	if (pipe(p) < 0)
		die("pipe nonblock");

	set_nonblock(p[0], "fcntl pipe nonblock read");
	expect_eagain_read(p[0], "nonblocking pipe read on empty live pipe");
	if (write(p[1], "N", 1) != 1)
		die("pipe nonblock write");
	expect_select_readable(p[0],
			       "nonblocking pipe read end still select-readable when data queued");
	expect_poll_ready(p[0], POLLIN, POLLIN, POLLHUP,
			  "nonblocking pipe read end still poll-readable when data queued");
	{
		char ch = 0;
		if (read(p[0], &ch, 1) != 1 || ch != 'N') {
			fprintf(stderr, "nonblocking pipe payload mismatch\n");
			exit(1);
		}
	}
	expect_eagain_read(p[0], "nonblocking pipe drained read returns EAGAIN");
	if (close(p[1]) < 0)
		die("close nonblock pipe writer");
	expect_eof_read(p[0], "nonblocking pipe read returns EOF after writer close");
	if (close(p[0]) < 0)
		die("close nonblock pipe reader");

	if (pipe(p) < 0)
		die("pipe nonblock epipe");
	if (close(p[0]) < 0)
		die("close nonblock pipe read end");
	set_nonblock(p[1], "fcntl pipe nonblock write");
	expect_poll_ready(p[1], POLLOUT, POLLERR, POLLOUT | POLLHUP,
			  "nonblocking pipe writer with no readers reports POLLERR");
	signal(SIGPIPE, SIG_IGN);
	expect_epipe_write(p[1], "nonblocking pipe writer returns EPIPE with no readers");
	if (close(p[1]) < 0)
		die("close nonblock pipe writer");
}

static void test_pipe_blocking_waits(void)
{
	int p[2];
	pid_t pid;
	char ch = 0;

	if (pipe(p) < 0)
		die("pipe blocking select");
	pid = fork();
	if (pid < 0)
		die("fork pipe blocking select");
	if (pid == 0) {
		usleep(100000);
		if (write(p[1], "B", 1) != 1)
			_exit(1);
		_exit(0);
	}
	close(p[1]);
	expect_select_readable_blocking(
		p[0], 2000,
		"blocking select wakes for pipe readability after writer activity");
	if (read(p[0], &ch, 1) != 1 || ch != 'B') {
		fprintf(stderr, "blocking select pipe payload mismatch\n");
		exit(1);
	}
	if (close(p[0]) < 0)
		die("close blocking select pipe read");
	wait_child_ok(pid, "blocking select pipe");

	if (pipe(p) < 0)
		die("pipe blocking poll");
	pid = fork();
	if (pid < 0)
		die("fork pipe blocking poll");
	if (pid == 0) {
		usleep(100000);
		close(p[1]);
		_exit(0);
	}
	close(p[1]);
	expect_poll_ready_blocking(
		p[0], POLLIN, 2000, POLLHUP, POLLIN,
		"blocking poll wakes for pipe EOF with POLLHUP-only");
	expect_eof_read(p[0], "blocking pipe poll EOF read");
	if (close(p[0]) < 0)
		die("close blocking poll pipe read");
	wait_child_ok(pid, "blocking poll pipe");
}

static void test_pty(void)
{
	int master, slave;
	char ch = 0;

	open_pts_pair(&master, &slave);
	set_raw_mode(slave);

	expect_select_not_readable(master, "pty master initially not readable");
	expect_select_writable(master, "pty master initially writable");
	expect_select_writable(slave, "pty slave initially writable");
	expect_select_not_excepted(master, "pty master initially no except");
	expect_select_not_excepted(slave, "pty slave initially no except");
	expect_poll_not_ready(master, POLLIN, "pty master initially poll timeout");
	expect_poll_ready(master, POLLOUT, POLLOUT, 0,
			  "pty master initially poll writable");
	expect_poll_ready(slave, POLLOUT, POLLOUT, 0,
			  "pty slave initially poll writable");

	if (write(slave, "M", 1) != 1)
		die("write slave");
	expect_select_readable(master, "pty master readable after slave write");
	expect_select_not_excepted(master,
				   "pty master readable path has no except");
	expect_poll_ready(master, POLLIN, POLLIN, POLLHUP,
			  "pty master poll readable after slave write");
	if (read(master, &ch, 1) != 1 || ch != 'M') {
		fprintf(stderr, "pty slave->master payload mismatch\n");
		exit(1);
	}

	if (write(master, "S", 1) != 1)
		die("write master");
	expect_select_readable(slave, "pty slave readable after master write");
	expect_select_not_excepted(slave, "pty slave readable path has no except");
	expect_poll_ready(slave, POLLIN, POLLIN, POLLHUP,
			  "pty slave poll readable after master write");
	if (read(slave, &ch, 1) != 1 || ch != 'S') {
		fprintf(stderr, "pty master->slave payload mismatch\n");
		exit(1);
	}

	if (close(slave) < 0)
		die("close pty slave");
	if (close(master) < 0)
		die("close pty master");
}

static void test_socket_stream(void)
{
	int sv[2];
	char ch = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		die("socketpair stream");

	expect_select_not_readable(sv[0], "stream socket initially not readable");
	expect_select_writable(sv[0], "stream socket end0 initially writable");
	expect_select_writable(sv[1], "stream socket end1 initially writable");
	expect_select_not_excepted(sv[0], "stream socket end0 initially no except");
	expect_select_not_excepted(sv[1], "stream socket end1 initially no except");
	expect_poll_not_ready(sv[0], POLLIN,
			      "stream socket initially poll timeout");
	expect_poll_ready(sv[0], POLLOUT, POLLOUT, 0,
			  "stream socket end0 initially poll writable");
	expect_poll_ready(sv[1], POLLOUT, POLLOUT, 0,
			  "stream socket end1 initially poll writable");

	if (write(sv[1], "Z", 1) != 1)
		die("stream socket write");
	expect_select_readable(sv[0],
			       "stream socket readable after peer write");
	expect_select_not_excepted(sv[0],
				   "stream socket readable path has no except");
	expect_poll_ready(sv[0], POLLIN, POLLIN, POLLHUP,
			  "stream socket poll readable after peer write");
	if (read(sv[0], &ch, 1) != 1 || ch != 'Z') {
		fprintf(stderr, "stream socket payload mismatch\n");
		exit(1);
	}

	if (close(sv[1]) < 0)
		die("close stream peer");
	expect_select_readable(sv[0], "stream socket EOF is select-readable");
	expect_select_not_excepted(sv[0], "stream socket EOF is not select-except");
	expect_poll_ready(sv[0], POLLIN, POLLIN | POLLHUP, 0,
			  "stream socket EOF is poll read+hup");
	expect_poll_exact(sv[0], 0, POLLHUP,
			  "stream socket EOF returns POLLHUP with no events requested");
	if (read(sv[0], &ch, 1) != 0) {
		fprintf(stderr, "stream socket EOF read mismatch\n");
		exit(1);
	}
	expect_select_readable(sv[0],
			       "stream socket remains select-readable after EOF drain");
	expect_poll_ready(sv[0], POLLIN, POLLIN | POLLHUP, 0,
			  "stream socket remains poll read+hup after EOF drain");

	if (close(sv[0]) < 0)
		die("close stream socket");
}

static void test_socket_stream_nonblocking_io(void)
{
	int sv[2];
	char ch = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		die("socketpair nonblock");

	set_nonblock(sv[0], "fcntl socketpair nonblock");
	expect_eagain_read(sv[0],
			   "nonblocking stream socket read on empty live peer returns EAGAIN");
	if (write(sv[1], "U", 1) != 1)
		die("socketpair nonblock peer write");
	expect_select_readable(
		sv[0],
		"nonblocking stream socket remains select-readable when data queued");
	expect_poll_ready(sv[0], POLLIN, POLLIN, POLLHUP,
			  "nonblocking stream socket remains poll-readable when data queued");
	if (read(sv[0], &ch, 1) != 1 || ch != 'U') {
		fprintf(stderr, "nonblocking stream socket payload mismatch\n");
		exit(1);
	}
	expect_eagain_read(sv[0],
			   "nonblocking stream socket drained read returns EAGAIN");

	if (close(sv[1]) < 0)
		die("close socketpair peer");
	expect_eof_read(sv[0],
			"nonblocking stream socket read returns EOF after peer close");
	signal(SIGPIPE, SIG_IGN);
	expect_epipe_write(sv[0],
			   "nonblocking stream socket write returns EPIPE after peer close");
	if (close(sv[0]) < 0)
		die("close nonblock stream socket");
}

static void test_socket_stream_blocking_waits(void)
{
	int sv[2];
	pid_t pid;
	char ch = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		die("socketpair blocking");
	pid = fork();
	if (pid < 0)
		die("fork socketpair blocking");
	if (pid == 0) {
		close(sv[0]);
		usleep(100000);
		if (write(sv[1], "W", 1) != 1)
			_exit(1);
		_exit(0);
	}
	close(sv[1]);
	expect_select_readable_blocking_prompt(
		sv[0], 2000, 1000,
		"blocking select wakes promptly for stream socket readability after peer write");
	if (read(sv[0], &ch, 1) != 1 || ch != 'W') {
		fprintf(stderr, "blocking stream socket select payload mismatch\n");
		exit(1);
	}
	if (close(sv[0]) < 0)
		die("close blocking stream socket select");
	wait_child_ok(pid, "blocking stream socket select");

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		die("socketpair blocking poll");
	pid = fork();
	if (pid < 0)
		die("fork socketpair blocking poll");
	if (pid == 0) {
		close(sv[0]);
		usleep(100000);
		if (write(sv[1], "X", 1) != 1)
			_exit(1);
		_exit(0);
	}
	close(sv[1]);
	expect_poll_ready_blocking_prompt(
		sv[0], POLLIN, 2000, 1000, POLLIN, 0,
		"blocking poll wakes promptly for stream socket readability after peer write");
	if (read(sv[0], &ch, 1) != 1 || ch != 'X') {
		fprintf(stderr, "blocking stream socket poll payload mismatch\n");
		exit(1);
	}
	if (close(sv[0]) < 0)
		die("close blocking stream socket poll");
	wait_child_ok(pid, "blocking stream socket poll");
}

static void test_named_unix_stream(void)
{
	char path[256];
	struct sockaddr_un addr;
	int listenfd;
	int clientfd;
	int connfd;
	int ack_pipe[2];
	pid_t pid;
	char ch = 0;

	snprintf(path, sizeof(path), "%s/named-stream.sock",
		 "/root/tests/posix_poll_matrix");
	unlink(path);

	listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listenfd < 0)
		die("named unix listen socket");
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("named unix bind");
	if (listen(listenfd, 1) < 0)
		die("named unix listen");

	expect_select_not_readable(listenfd,
				   "named AF_UNIX listener initially not readable");
	expect_select_not_excepted(listenfd,
				   "named AF_UNIX listener initially no except");
	expect_poll_not_ready(listenfd, POLLIN,
			      "named AF_UNIX listener initially not poll-readable");

	if (pipe(ack_pipe) < 0)
		die("named unix ack pipe");
	pid = fork();
	if (pid < 0)
		die("fork named unix");
	if (pid == 0) {
		close(ack_pipe[0]);
		clientfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (clientfd < 0)
			_exit(1);
		if (connect(clientfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
			_exit(2);
		if (write(ack_pipe[1], "C", 1) != 1)
			_exit(3);
		usleep(100000);
		if (write(clientfd, "Y", 1) != 1)
			_exit(4);
		usleep(100000);
		close(clientfd);
		close(ack_pipe[1]);
		_exit(0);
	}

	close(ack_pipe[1]);
	expect_select_readable_blocking(
		listenfd, 2000,
		"blocking select wakes when named AF_UNIX listener has pending accept");
	connfd = accept(listenfd, NULL, NULL);
	if (connfd < 0)
		die("named unix accept");
	{
		char ack = 0;
		if (read(ack_pipe[0], &ack, 1) != 1 || ack != 'C')
			die("named unix ack read");
	}
	close(ack_pipe[0]);

	set_nonblock(connfd, "fcntl named unix accepted conn nonblock");
	expect_eagain_read(connfd,
			   "nonblocking named AF_UNIX accepted socket returns EAGAIN while peer open and idle");
	expect_select_writable(connfd,
			       "named AF_UNIX accepted socket initially writable");
	expect_select_not_excepted(connfd,
				   "named AF_UNIX accepted socket initially no except");
	expect_poll_ready(connfd, POLLOUT, POLLOUT, 0,
			  "named AF_UNIX accepted socket initially poll writable");

	expect_poll_ready_blocking(
		connfd, POLLIN, 2000, POLLIN, POLLHUP,
		"blocking poll wakes when named AF_UNIX peer sends data");
	if (read(connfd, &ch, 1) != 1 || ch != 'Y') {
		fprintf(stderr, "named AF_UNIX payload mismatch\n");
		exit(1);
	}

	expect_poll_ready_blocking(
		connfd, POLLIN, 2000, POLLIN | POLLHUP, 0,
		"blocking poll wakes on named AF_UNIX peer close with read+hup");
	expect_select_readable(connfd,
			       "named AF_UNIX EOF is select-readable on accepted socket");
	expect_eof_read(connfd, "named AF_UNIX accepted socket EOF read");
	signal(SIGPIPE, SIG_IGN);
	expect_epipe_write(connfd,
			   "named AF_UNIX accepted socket write returns EPIPE after peer close");

	if (close(connfd) < 0)
		die("close named unix conn");
	if (close(listenfd) < 0)
		die("close named unix listener");
	wait_child_ok(pid, "named AF_UNIX stream");
	unlink(path);
}

static void test_controlling_tty(void)
{
	int master, slave;
	int cmd_pipe[2];
	int ack_pipe[2];
	pid_t pid;
	int status;

	open_pts_pair(&master, &slave);
	set_raw_mode(slave);

	if (pipe(cmd_pipe) < 0)
		die("pipe cmd");
	if (pipe(ack_pipe) < 0)
		die("pipe ack");

	pid = fork();
	if (pid < 0)
		die("fork tty");
	if (pid == 0) {
		int ttyfd;
		char cmd = 0;
		char ch = 0;

		close(cmd_pipe[1]);
		close(ack_pipe[0]);
		close(master);

		if (setsid() < 0)
			die("setsid");
		if (ioctl(slave, TIOCSCTTY, 0) != 0)
			die("TIOCSCTTY");
		ttyfd = open("/dev/tty", O_RDWR);
		if (ttyfd < 0)
			die("open /dev/tty");
		set_raw_mode(ttyfd);

		expect_select_writable(ttyfd, "/dev/tty initially writable");
		expect_select_not_readable(ttyfd, "/dev/tty initially not readable");
		expect_select_not_excepted(ttyfd, "/dev/tty initially no except");
		expect_poll_ready(ttyfd, POLLOUT, POLLOUT, 0,
				  "/dev/tty initially poll writable");
		expect_poll_not_ready(ttyfd, POLLIN,
				      "/dev/tty initially not poll readable");

		if (write(ack_pipe[1], "R", 1) != 1)
			die("ack ready");
		if (read(cmd_pipe[0], &cmd, 1) != 1 || cmd != 'G')
			die("cmd read");

		expect_select_readable(ttyfd, "/dev/tty readable after master write");
		expect_select_not_excepted(ttyfd,
					   "/dev/tty readable path has no except");
		expect_poll_ready(ttyfd, POLLIN, POLLIN, POLLHUP,
				  "/dev/tty poll readable after master write");
		if (read(ttyfd, &ch, 1) != 1 || ch != 'T') {
			fprintf(stderr, "/dev/tty payload mismatch\n");
			_exit(1);
		}

		close(ttyfd);
		close(slave);
		close(cmd_pipe[0]);
		close(ack_pipe[1]);
		_exit(0);
	}

	close(cmd_pipe[0]);
	close(ack_pipe[1]);
	close(slave);

	{
		char ack = 0;
		if (read(ack_pipe[0], &ack, 1) != 1 || ack != 'R')
			die("ack wait");
	}
	if (write(master, "T", 1) != 1)
		die("write master tty");
	if (write(cmd_pipe[1], "G", 1) != 1)
		die("send cmd");

	close(cmd_pipe[1]);
	close(ack_pipe[0]);
	if (waitpid(pid, &status, 0) < 0)
		die("waitpid tty");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "tty child failed: status=%d\n", status);
		exit(1);
	}

	if (close(master) < 0)
		die("close tty master");
}

int main(void)
{
	test_zero_fd_calls();
	test_invalid_fd();
	test_ext4_file();
	test_pipe();
	test_pipe_nonblocking_io();
	test_pipe_blocking_waits();
	test_pty();
	test_socket_stream();
	test_socket_stream_nonblocking_io();
	test_socket_stream_blocking_waits();
	test_named_unix_stream();
	test_controlling_tty();
	return 0;
}
EOF

expect_success gcc -Wall -Werror -o "$BIN" "$SRC"
update_case_timestamp
fi
expect_success "$BIN"
