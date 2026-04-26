#!/bin/sh

set -e
CASE_NAME=posix_fcntl
CASE_MTIME=@CASE_MTIME@
BASE=/root/tests/$CASE_NAME
WORKDIR=/root/tests/$CASE_NAME
SRC="$BASE/fcntl_test.c"
BIN="$BASE/fcntl_test"
STAMP="$BASE/.case_timestamp"

fail()
{
	echo "posix_fcntl: $1" >&2
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
cat > "$SRC" << 'EOF'
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define FILE_PATH "/root/tests/posix_fcntl/data.txt"

static int fail(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	return 1;
}

static int fail_errno(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	return 1;
}

/* ── O_APPEND test ────────────────────────────────────────────────────────── */
static int test_o_append(void)
{
	int fd;
	char buf[64];
	ssize_t n;

	unlink(FILE_PATH);
	fd = open(FILE_PATH, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0)
		return fail_errno("open initial");

	/* Write initial content. */
	if (write(fd, "AAA", 3) != 3) { close(fd); return fail_errno("write AAA"); }
	close(fd);

	/* Reopen with O_APPEND. */
	fd = open(FILE_PATH, O_RDWR | O_APPEND);
	if (fd < 0)
		return fail_errno("open O_APPEND");

	/* Seek to beginning — write should still go to end due to O_APPEND. */
	if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return fail_errno("lseek 0"); }
	if (write(fd, "BBB", 3) != 3) { close(fd); return fail_errno("write BBB"); }

	/* Read back from start. */
	if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return fail_errno("lseek readback"); }
	memset(buf, 0, sizeof(buf));
	n = read(fd, buf, 6);
	close(fd);

	if (n != 6)
	{
		fprintf(stderr, "O_APPEND: read %ld bytes, expected 6\n", (long)n);
		return 1;
	}
	if (memcmp(buf, "AAABBB", 6) != 0)
	{
		buf[6] = '\0';
		fprintf(stderr, "O_APPEND: file content '%s', expected 'AAABBB'\n", buf);
		return 1;
	}
	return 0;
}

/* ── F_GETFL / F_SETFL test ──────────────────────────────────────────────── */
static int test_getfl_setfl(void)
{
	int fd, flags;

	unlink(FILE_PATH);
	fd = open(FILE_PATH, O_CREAT | O_RDWR | O_APPEND, 0600);
	if (fd < 0)
		return fail_errno("open for getfl");

	flags = fcntl(fd, F_GETFL);
	if (flags < 0) { close(fd); return fail_errno("F_GETFL"); }

	if (!(flags & O_APPEND)) {
		close(fd);
		return fail("F_GETFL: O_APPEND not set on O_APPEND-opened fd");
	}
	if (!(flags & O_RDWR)) {
		close(fd);
		return fail("F_GETFL: O_RDWR not reported");
	}

	/* Clear O_APPEND via F_SETFL */
	if (fcntl(fd, F_SETFL, flags & ~O_APPEND) < 0) {
		close(fd);
		return fail_errno("F_SETFL clear O_APPEND");
	}
	flags = fcntl(fd, F_GETFL);
	if (flags < 0) { close(fd); return fail_errno("F_GETFL after clear"); }
	if (flags & O_APPEND) {
		close(fd);
		return fail("F_SETFL: O_APPEND still set after clearing");
	}

	/* Restore O_APPEND via F_SETFL */
	if (fcntl(fd, F_SETFL, flags | O_APPEND) < 0) {
		close(fd);
		return fail_errno("F_SETFL set O_APPEND");
	}
	flags = fcntl(fd, F_GETFL);
	if (flags < 0) { close(fd); return fail_errno("F_GETFL after restore"); }
	if (!(flags & O_APPEND)) {
		close(fd);
		return fail("F_SETFL: O_APPEND not set after restoring");
	}

	close(fd);
	return 0;
}

/* ── pread / pwrite test ─────────────────────────────────────────────────── */
static int test_pread_pwrite(void)
{
	int fd;
	char buf[16];
	ssize_t n;
	off_t pos;

	unlink(FILE_PATH);
	fd = open(FILE_PATH, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0)
		return fail_errno("open for pread/pwrite");

	/* Fill with zeros. */
	memset(buf, 0, sizeof(buf));
	if (write(fd, buf, 16) != 16) { close(fd); return fail_errno("initial write"); }

	/* pwrite at offset 4: should not move the file offset. */
	if (pwrite(fd, "XXXX", 4, 4) != 4) { close(fd); return fail_errno("pwrite at 4"); }

	pos = lseek(fd, 0, SEEK_CUR);
	if (pos != 16) {
		close(fd);
		fprintf(stderr, "pwrite moved file offset to %ld, expected 16\n", (long)pos);
		return 1;
	}

	/* pread at offset 4: should not move the file offset. */
	memset(buf, 0, sizeof(buf));
	n = pread(fd, buf, 4, 4);
	if (n != 4) { close(fd); return fail_errno("pread at 4"); }
	if (memcmp(buf, "XXXX", 4) != 0) {
		close(fd);
		return fail("pread at 4: did not read back XXXX");
	}

	pos = lseek(fd, 0, SEEK_CUR);
	if (pos != 16) {
		close(fd);
		fprintf(stderr, "pread moved file offset to %ld, expected 16\n", (long)pos);
		return 1;
	}

	/* pwrite at offset 0: first 4 bytes. */
	if (pwrite(fd, "YYYY", 4, 0) != 4) { close(fd); return fail_errno("pwrite at 0"); }

	/* pread full content and verify layout: YYYY XXXX zeros */
	memset(buf, 0, sizeof(buf));
	n = pread(fd, buf, 16, 0);
	if (n != 16) { close(fd); return fail_errno("pread full"); }
	if (memcmp(buf, "YYYY", 4) != 0) {
		close(fd);
		return fail("pread full: bytes 0-3 not YYYY");
	}
	if (memcmp(buf + 4, "XXXX", 4) != 0) {
		close(fd);
		return fail("pread full: bytes 4-7 not XXXX");
	}

	close(fd);
	return 0;
}

/* ── dup / dup2 test ─────────────────────────────────────────────────────── */
static int test_dup(void)
{
	int fd, fd2, fd3;
	char buf[8];

	unlink(FILE_PATH);
	fd = open(FILE_PATH, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0)
		return fail_errno("open for dup");

	if (write(fd, "hello", 5) != 5) { close(fd); return fail_errno("write hello"); }

	/* dup: both fds share the same file offset */
	fd2 = dup(fd);
	if (fd2 < 0) { close(fd); return fail_errno("dup"); }
	if (fd2 == fd) { close(fd); close(fd2); return fail("dup returned same fd"); }

	/* Seek fd2 to start; read 5 bytes through fd2 */
	if (lseek(fd2, 0, SEEK_SET) < 0) { close(fd); close(fd2); return fail_errno("lseek dup"); }
	memset(buf, 0, sizeof(buf));
	if (read(fd2, buf, 5) != 5) { close(fd); close(fd2); return fail_errno("read via dup"); }
	if (memcmp(buf, "hello", 5) != 0) {
		close(fd); close(fd2);
		return fail("dup: read back wrong data");
	}

	/* dup2: redirect to a specific fd number */
	fd3 = fd2 + 10;
	if (dup2(fd, fd3) != fd3) { close(fd); close(fd2); return fail_errno("dup2"); }

	if (lseek(fd3, 0, SEEK_SET) < 0) { close(fd); close(fd2); close(fd3); return fail_errno("lseek dup2"); }
	memset(buf, 0, sizeof(buf));
	if (read(fd3, buf, 5) != 5) { close(fd); close(fd2); close(fd3); return fail_errno("read via dup2"); }
	if (memcmp(buf, "hello", 5) != 0) {
		close(fd); close(fd2); close(fd3);
		return fail("dup2: read back wrong data");
	}

	close(fd);
	close(fd2);
	close(fd3);
	return 0;
}

int main(void)
{
	int rc = 0;

	if (test_o_append()    != 0) { fprintf(stderr, "FAIL: o_append\n");    rc = 1; }
	if (test_getfl_setfl() != 0) { fprintf(stderr, "FAIL: getfl_setfl\n"); rc = 1; }
	if (test_pread_pwrite()!= 0) { fprintf(stderr, "FAIL: pread_pwrite\n");rc = 1; }
	if (test_dup()         != 0) { fprintf(stderr, "FAIL: dup\n");         rc = 1; }

	if (rc == 0)
		printf("all fcntl tests passed\n");
	return rc;
}
EOF

expect_success gcc -o "$BIN" "$SRC"
update_case_timestamp
fi
[ -x "$BIN" ] || fail "compiled binary missing"
expect_success "$BIN"
