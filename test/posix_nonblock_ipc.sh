#!/bin/sh

set -e
BASE=/root/posix_nonblock_ipc
SRC="$BASE/nonblock_ipc.c"
BIN="$BASE/nonblock_ipc"

fail()
{
	echo "posix_nonblock_ipc: $1" >&2
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

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

require_cmd gcc
require_cmd grep

expect_success_sh "grep -q '^devpts /dev/pts ' /proc/mounts"

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"

cat > "$SRC" <<'EOF'
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
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

static void expect_eagain(int fd, const char *label)
{
	char ch = 0;
	ssize_t n = read(fd, &ch, 1);

	if (n != -1 || errno != EAGAIN) {
		fprintf(stderr, "%s: expected -1/EAGAIN, got n=%ld errno=%d\n",
			label, (long)n, errno);
		exit(1);
	}
}

static void expect_eof(int fd, const char *label)
{
	char ch = 0;
	ssize_t n = read(fd, &ch, 1);

	if (n != 0) {
		fprintf(stderr, "%s: expected EOF, got n=%ld errno=%d\n",
			label, (long)n, errno);
		exit(1);
	}
}

static void test_pipe_nonblock(void)
{
	int p[2];
	int flags;

	if (pipe(p) < 0)
		die("pipe");
	flags = fcntl(p[0], F_GETFL, 0);
	if (flags < 0)
		die("fcntl pipe read getfl");
	if (fcntl(p[0], F_SETFL, flags | O_NONBLOCK) < 0)
		die("fcntl pipe read setfl");

	expect_eagain(p[0], "pipe empty read while writer open");

	if (write(p[1], "Q", 1) != 1)
		die("pipe write");

	{
		char ch = 0;
		ssize_t n = read(p[0], &ch, 1);
		if (n != 1 || ch != 'Q') {
			fprintf(stderr,
				"pipe payload mismatch: n=%ld ch=%d\n",
				(long)n, (int)ch);
			exit(1);
		}
	}

	expect_eagain(p[0], "pipe empty after drain while writer open");

	if (close(p[1]) < 0)
		die("close pipe writer");
	expect_eof(p[0], "pipe EOF after writer close");
	if (close(p[0]) < 0)
		die("close pipe reader");
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

static void make_nonblock(int fd, const char *label)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		die(label);
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		die(label);
}

static void set_raw_mode(int slave)
{
	struct termios tio;

	if (ioctl(slave, TCGETS, &tio) != 0)
		die("TCGETS");
	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_lflag = 0;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	if (ioctl(slave, TCSETS, &tio) != 0)
		die("TCSETS");
}

static void test_pts_nonblock(void)
{
	int master, slave;

	open_pts_pair(&master, &slave);
	make_nonblock(master, "fcntl master nonblock");
	make_nonblock(slave, "fcntl slave nonblock");
	set_raw_mode(slave);

	expect_eagain(master, "pty master empty read while slave open");
	expect_eagain(slave, "pty slave empty read while master open");

	if (write(master, "A", 1) != 1)
		die("write master");
	{
		char ch = 0;
		ssize_t n = read(slave, &ch, 1);
		if (n != 1 || ch != 'A') {
			fprintf(stderr,
				"pty master->slave mismatch: n=%ld ch=%d\n",
				(long)n, (int)ch);
			exit(1);
		}
	}
	expect_eagain(slave, "pty slave empty after drain while master open");

	if (write(slave, "B", 1) != 1)
		die("write slave");
	{
		char ch = 0;
		ssize_t n = read(master, &ch, 1);
		if (n != 1 || ch != 'B') {
			fprintf(stderr,
				"pty slave->master mismatch: n=%ld ch=%d\n",
				(long)n, (int)ch);
			exit(1);
		}
	}
	expect_eagain(master, "pty master empty after drain while slave open");

	if (close(slave) < 0)
		die("close slave");
	expect_eof(master, "pty master EOF after slave close");
	if (close(master) < 0)
		die("close master");

	open_pts_pair(&master, &slave);
	make_nonblock(slave, "fcntl slave nonblock reopen");
	set_raw_mode(slave);
	if (close(master) < 0)
		die("close master reopen");
	expect_eof(slave, "pty slave EOF after master close");
	if (close(slave) < 0)
		die("close slave reopen");
}

int main(void)
{
	test_pipe_nonblock();
	test_pts_nonblock();
	return 0;
}
EOF

expect_success gcc -Wall -Werror -o "$BIN" "$SRC"
expect_success "$BIN"
