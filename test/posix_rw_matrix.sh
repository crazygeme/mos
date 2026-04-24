#!/bin/sh

set -e
CASE_NAME=posix_rw_matrix
CASE_MTIME=@CASE_MTIME@
BASE=/root/tests/$CASE_NAME
WORKDIR=/root/posix_rw_matrix
SRC="$BASE/rw_matrix.c"
BIN="$BASE/rw_matrix"
STAMP="$BASE/.case_timestamp"

fail()
{
	echo "posix_rw_matrix: $1" >&2
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
#include <sys/resource.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void failf(const char *label, const char *detail)
{
	fprintf(stderr, "%s: %s\n", label, detail);
	exit(1);
}

static void expect_errno(int actual, int want1, int want2, const char *label)
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
		fprintf(stderr, "%s: child status=%d\n", label, status);
		exit(1);
	}
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

static void expect_short_read(int fd, void *buf, size_t want_count,
			      size_t max_count, const char *label)
{
	ssize_t n;

	n = read(fd, buf, max_count);
	if (n != (ssize_t)want_count) {
		fprintf(stderr,
			"%s: expected short read %lu, got %ld errno=%d\n",
			label, (unsigned long)want_count, (long)n, errno);
		exit(1);
	}
}

static void expect_read_zero(int fd, const char *label)
{
	char ch = 0;
	ssize_t n = read(fd, &ch, 1);

	if (n != 0) {
		fprintf(stderr, "%s: expected EOF/0, got %ld errno=%d\n", label,
			(long)n, errno);
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
	expect_errno(errno, want1, want2, label);
}

static void expect_write_exact(int fd, const void *buf, size_t len,
			       const char *label)
{
	ssize_t n = write(fd, buf, len);

	if (n != (ssize_t)len) {
		fprintf(stderr, "%s: expected write %lu, got %ld errno=%d\n",
			label, (unsigned long)len, (long)n, errno);
		exit(1);
	}
}

static void expect_write_zero(int fd, const char *label)
{
	ssize_t n = write(fd, "", 0);

	if (n != 0) {
		fprintf(stderr, "%s: expected 0-byte write to return 0, got %ld errno=%d\n",
			label, (long)n, errno);
		exit(1);
	}
}

static void expect_write_errno(int fd, int want1, int want2,
			       const char *label)
{
	ssize_t n;

	errno = 0;
	n = write(fd, "Z", 1);
	if (n != -1) {
		fprintf(stderr, "%s: expected -1, got %ld\n", label, (long)n);
		exit(1);
	}
	expect_errno(errno, want1, want2, label);
}

static void fill_pipe_until_eagain(int fd)
{
	char buf[512];
	ssize_t n;

	memset(buf, 'P', sizeof(buf));
	set_nonblock(fd, 1, "fcntl pipe nonblock fill");
	for (;;) {
		n = write(fd, buf, sizeof(buf));
		if (n == (ssize_t)sizeof(buf))
			continue;
		if (n == -1 && errno == EAGAIN)
			break;
		fprintf(stderr, "fill_pipe_until_eagain: n=%ld errno=%d\n",
			(long)n, errno);
		exit(1);
	}
	set_nonblock(fd, 0, "fcntl pipe blocking restore");
}

static void make_named_unix_stream_pair(const char *path, int *server_fd,
					int *client_fd, int *listen_fd)
{
	struct sockaddr_un addr;

	unlink(path);
	*listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (*listen_fd < 0)
		die("socket listen");

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (bind(*listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("bind unix");
	if (listen(*listen_fd, 1) < 0)
		die("listen unix");

	*client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (*client_fd < 0)
		die("socket client");
	if (connect(*client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("connect unix");

	*server_fd = accept(*listen_fd, NULL, NULL);
	if (*server_fd < 0)
		die("accept unix");
}

static void test_regular_file_basics(void)
{
	char path[256];
	int fd;
	char buf[16];
	struct stat st;
	off_t off;

	snprintf(path, sizeof(path), "%s/file.txt", "/root/posix_rw_matrix");
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0)
		die("open regular file");

	expect_write_exact(fd, "hello", 5, "regular file write");
	off = lseek(fd, 0, SEEK_CUR);
	if (off != 5)
		failf("regular file write", "file offset did not advance");

	if (lseek(fd, 0, SEEK_SET) < 0)
		die("lseek regular file");
	memset(buf, 0, sizeof(buf));
	expect_short_read(fd, buf, 5, 8, "regular file short read at EOF");
	if (memcmp(buf, "hello", 5) != 0)
		failf("regular file short read", "payload mismatch");
	expect_read_zero(fd, "regular file EOF read returns 0");

	if (lseek(fd, 2, SEEK_SET) < 0)
		die("lseek regular file zero read");
	off = lseek(fd, 0, SEEK_CUR);
	if (off != 2)
		failf("regular file zero read", "precondition offset mismatch");
	if (read(fd, buf, 0) != 0)
		failf("regular file zero read", "count-0 read did not return 0");
	if (lseek(fd, 0, SEEK_CUR) != off)
		failf("regular file zero read", "count-0 read changed file offset");
	if (fstat(fd, &st) != 0)
		die("fstat regular file before zero write");
	expect_write_zero(fd, "regular file zero write");
	{
		struct stat st_after;
		if (fstat(fd, &st_after) != 0)
			die("fstat regular file after zero write");
		if (st_after.st_size != st.st_size)
			failf("regular file zero write", "count-0 write changed file size");
	}

	set_nonblock(fd, 1, "fcntl regular file nonblock");
	if (lseek(fd, 0, SEEK_SET) < 0)
		die("lseek regular file nonblock");
	expect_read_exact(fd, "hello", 5,
			  "regular file O_NONBLOCK read behaves like normal read");
	expect_write_exact(fd, "!", 1,
			   "regular file O_NONBLOCK write behaves like normal write");
	if (close(fd) < 0)
		die("close regular file");
}

static void test_regular_file_errors(void)
{
	char path[256];
	int fd_rdonly, fd_wronly, fd_dir, fd_closed;

	snprintf(path, sizeof(path), "%s/error.txt", "/root/posix_rw_matrix");
	fd_rdonly = open(path, O_CREAT | O_TRUNC | O_RDONLY, 0644);
	if (fd_rdonly < 0)
		die("open rdonly");
	fd_wronly = open(path, O_WRONLY, 0644);
	if (fd_wronly < 0)
		die("open wronly");

	expect_write_errno(fd_rdonly, EBADF, -1,
			   "write on O_RDONLY regular file returns EBADF");
	expect_read_errno(fd_wronly, EBADF, -1,
			  "read on O_WRONLY regular file returns EBADF");

	fd_closed = open(path, O_RDONLY, 0644);
	if (fd_closed < 0)
		die("open closed-fd");
	if (close(fd_closed) < 0)
		die("close closed-fd");
	expect_read_errno(fd_closed, EBADF, -1, "read on closed fd returns EBADF");
	expect_write_errno(fd_closed, EBADF, -1,
			   "write on closed fd returns EBADF");

	fd_dir = open("/root", O_RDONLY);
	if (fd_dir < 0)
		die("open directory");
	expect_read_errno(fd_dir, EISDIR, -1, "read on directory returns EISDIR");

	close(fd_dir);
	close(fd_wronly);
	close(fd_rdonly);
}

static void test_regular_file_rlimit_fsize(void)
{
	char path[256];
	pid_t pid;

	snprintf(path, sizeof(path), "%s/rlimit.txt", "/root/posix_rw_matrix");
	pid = fork();
	if (pid < 0)
		die("fork rlimit_fsize");
	if (pid == 0) {
		struct rlimit rl;
		char buf[512];
		int fd;
		ssize_t n;

		memset(buf, 'R', sizeof(buf));
		rl.rlim_cur = 256;
		rl.rlim_max = 256;
		if (setrlimit(RLIMIT_FSIZE, &rl) != 0)
			_exit(1);
		signal(SIGXFSZ, SIG_IGN);
		fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		if (fd < 0)
			_exit(2);

		n = write(fd, buf, sizeof(buf));
		if (n != 256)
			_exit(3);
		n = write(fd, buf, 1);
		if (n != -1 || errno != EFBIG)
			_exit(4);
		_exit(0);
	}
	wait_child_ok(pid, "regular file RLIMIT_FSIZE");
}

static void __attribute__((unused)) test_pipe_basic_and_eof(void)
{
	int p[2];
	char buf[8];

	if (pipe(p) < 0)
		die("pipe basic");
	expect_write_exact(p[1], "xy", 2, "pipe write success");
	memset(buf, 0, sizeof(buf));
	expect_short_read(p[0], buf, 2, sizeof(buf),
			  "pipe short read when only 2 bytes available");
	if (memcmp(buf, "xy", 2) != 0)
		failf("pipe short read", "payload mismatch");

	if (close(p[1]) < 0)
		die("close pipe writer");
	expect_read_zero(p[0], "pipe EOF read returns 0");
	if (close(p[0]) < 0)
		die("close pipe reader");
}

static void __attribute__((unused)) test_pipe_nonblocking(void)
{
	int p[2];

	if (pipe(p) < 0)
		die("pipe nonblock");
	set_nonblock(p[0], 1, "fcntl pipe read nonblock");
	expect_read_errno(p[0], EAGAIN, -1,
			  "nonblocking pipe read on empty live pipe returns EAGAIN");
	expect_write_exact(p[1], "n", 1, "nonblocking pipe peer write");
	expect_read_exact(p[0], "n", 1, "nonblocking pipe read success");

	fill_pipe_until_eagain(p[1]);
	set_nonblock(p[1], 1, "fcntl pipe write nonblock");
	expect_write_errno(p[1], EAGAIN, -1,
			   "nonblocking pipe write to full pipe returns EAGAIN");

	if (close(p[0]) < 0)
		die("close pipe reader for EPIPE");
	signal(SIGPIPE, SIG_IGN);
	expect_write_errno(p[1], EPIPE, -1,
			   "pipe write with no readers returns EPIPE");
	if (close(p[1]) < 0)
		die("close pipe writer after EPIPE");
}

static void __attribute__((unused)) test_pipe_blocking_read(void)
{
	int p[2];
	pid_t pid;
	char ch = 0;

	if (pipe(p) < 0)
		die("pipe blocking read");
	pid = fork();
	if (pid < 0)
		die("fork pipe blocking read");
	if (pid == 0) {
		close(p[0]);
		sleep(1);
		if (write(p[1], "B", 1) != 1)
			_exit(1);
		_exit(0);
	}
	close(p[1]);
	if (read(p[0], &ch, 1) != 1 || ch != 'B')
		failf("blocking pipe read", "did not wake and receive payload");
	close(p[0]);
	wait_child_ok(pid, "blocking pipe read");
}

static void __attribute__((unused)) test_named_unix_socket_rw(void)
{
	char path[256];
	int srv, cli, lst;
	char buf[8];

	snprintf(path, sizeof(path), "%s/stream.sock", "/root/posix_rw_matrix");
	make_named_unix_stream_pair(path, &srv, &cli, &lst);

	set_nonblock(srv, 1, "fcntl unix srv nonblock");
	expect_read_errno(srv, EAGAIN, EWOULDBLOCK,
			  "nonblocking AF_UNIX stream read on empty socket returns would-block");

	expect_write_exact(cli, "ab", 2, "AF_UNIX client write");
	memset(buf, 0, sizeof(buf));
	expect_short_read(srv, buf, 2, sizeof(buf),
			  "AF_UNIX server short read when 2 bytes queued");
	if (memcmp(buf, "ab", 2) != 0)
		failf("AF_UNIX server short read", "payload mismatch");

	expect_write_exact(srv, "cd", 2, "AF_UNIX server write");
	memset(buf, 0, sizeof(buf));
	expect_short_read(cli, buf, 2, sizeof(buf),
			  "AF_UNIX client short read when 2 bytes queued");
	if (memcmp(buf, "cd", 2) != 0)
		failf("AF_UNIX client short read", "payload mismatch");

	if (close(srv) < 0)
		die("close AF_UNIX server");
	expect_read_zero(cli, "AF_UNIX stream EOF read returns 0 after peer close");
	signal(SIGPIPE, SIG_IGN);
	expect_write_errno(cli, EPIPE, -1,
			   "AF_UNIX stream write after peer close returns EPIPE");

	close(cli);
	close(lst);
	unlink(path);
}

static void __attribute__((unused)) test_named_unix_socket_blocking_read(void)
{
	char path[256];
	int srv, cli, lst;
	pid_t pid;
	char ch = 0;

	snprintf(path, sizeof(path), "%s/blocking.sock", "/root/posix_rw_matrix");
	make_named_unix_stream_pair(path, &srv, &cli, &lst);

	pid = fork();
	if (pid < 0)
		die("fork unix blocking read");
	if (pid == 0) {
		close(srv);
		close(lst);
		sleep(1);
		if (write(cli, "K", 1) != 1)
			_exit(1);
		_exit(0);
	}
	close(cli);
	close(lst);
	if (read(srv, &ch, 1) != 1 || ch != 'K')
		failf("blocking AF_UNIX read", "did not wake and receive payload");
	close(srv);
	wait_child_ok(pid, "blocking AF_UNIX read");
	unlink(path);
}

static void __attribute__((unused)) test_pts_rw(void)
{
	int master, slave;
	char buf[8];

	open_pts_pair(&master, &slave);
	set_raw_mode(slave);
	set_nonblock(master, 1, "fcntl pts master nonblock");
	set_nonblock(slave, 1, "fcntl pts slave nonblock");

	expect_read_errno(master, EAGAIN, -1,
			  "nonblocking pty master read on empty returns EAGAIN");
	expect_read_errno(slave, EAGAIN, -1,
			  "nonblocking pty slave read on empty returns EAGAIN");

	expect_write_exact(master, "pq", 2, "pty master write");
	memset(buf, 0, sizeof(buf));
	expect_short_read(slave, buf, 2, sizeof(buf),
			  "pty slave short read when 2 bytes queued");
	if (memcmp(buf, "pq", 2) != 0)
		failf("pty slave short read", "payload mismatch");

	expect_write_exact(slave, "rs", 2, "pty slave write");
	memset(buf, 0, sizeof(buf));
	expect_short_read(master, buf, 2, sizeof(buf),
			  "pty master short read when 2 bytes queued");
	if (memcmp(buf, "rs", 2) != 0)
		failf("pty master short read", "payload mismatch");

	if (close(slave) < 0)
		die("close pty slave");
	expect_read_zero(master, "pty master EOF after slave close");
	close(master);
}

static void __attribute__((unused)) test_pts_blocking_read(void)
{
	int master, slave;
	pid_t pid;
	char ch = 0;

	open_pts_pair(&master, &slave);
	set_raw_mode(slave);
	pid = fork();
	if (pid < 0)
		die("fork pts blocking");
	if (pid == 0) {
		close(master);
		sleep(1);
		if (write(slave, "T", 1) != 1)
			_exit(1);
		_exit(0);
	}
	close(slave);
	if (read(master, &ch, 1) != 1 || ch != 'T')
		failf("blocking pty master read", "did not wake and receive payload");
	close(master);
	wait_child_ok(pid, "blocking pty master read");
}

static void __attribute__((unused)) test_controlling_tty_rw(void)
{
	int master, slave;
	int ready_pipe[2];
	pid_t pid;
	int status;

	open_pts_pair(&master, &slave);
	set_raw_mode(slave);
	if (pipe(ready_pipe) < 0)
		die("tty ready pipe");

	pid = fork();
	if (pid < 0)
		die("fork controlling tty");
	if (pid == 0) {
		int ttyfd;
		char buf[8];
		char ready = 'R';

		close(ready_pipe[0]);
		close(master);
		if (setsid() < 0)
			_exit(1);
		if (ioctl(slave, TIOCSCTTY, 0) != 0)
			_exit(2);
		ttyfd = open("/dev/tty", O_RDWR);
		if (ttyfd < 0)
			_exit(3);
		set_raw_mode(ttyfd);
		set_nonblock(ttyfd, 1, "fcntl /dev/tty nonblock");
		expect_read_errno(ttyfd, EAGAIN, -1,
				  "nonblocking /dev/tty read on empty returns EAGAIN");
		if (write(ready_pipe[1], &ready, 1) != 1)
			_exit(4);

		set_nonblock(ttyfd, 0, "fcntl /dev/tty blocking restore");
		memset(buf, 0, sizeof(buf));
		expect_short_read(ttyfd, buf, 2, sizeof(buf),
				  "blocking /dev/tty short read when 2 bytes queued");
		if (memcmp(buf, "UV", 2) != 0)
			_exit(5);
		expect_write_exact(ttyfd, "W", 1, "/dev/tty write success");
		close(ttyfd);
		close(slave);
		close(ready_pipe[1]);
		_exit(0);
	}

	close(ready_pipe[1]);
	close(slave);
	{
		char ready = 0;
		if (read(ready_pipe[0], &ready, 1) != 1 || ready != 'R')
			die("tty ready");
	}
	expect_write_exact(master, "UV", 2, "write to pty master for /dev/tty read");
	expect_read_exact(master, "W", 1, "read /dev/tty write from pty master");

	if (waitpid(pid, &status, 0) < 0)
		die("waitpid controlling tty");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "controlling tty child status=%d\n", status);
		exit(1);
	}
	close(ready_pipe[0]);
	close(master);
}

int main(void)
{
	test_regular_file_basics();
	test_regular_file_errors();
	test_regular_file_rlimit_fsize();
	test_pipe_basic_and_eof();
	test_pipe_blocking_read();
	test_pipe_nonblocking();
	test_named_unix_socket_rw();
	test_pts_rw();
	return 0;
}
EOF

expect_success gcc -Wall -Werror -o "$BIN" "$SRC"
update_case_timestamp
fi
expect_success "$BIN"
