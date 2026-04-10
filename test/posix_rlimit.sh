#!/bin/sh

set -e
BASE=/root/posix_rlimit
SRC="$BASE/rlimit_test.c"
BIN="$BASE/rlimit_test"

fail()
{
	echo "posix_rlimit: $1" >&2
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

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

require_cmd gcc

mkdir -p "$BASE"

cat > "$SRC" << 'EOF'
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define TMPFILE "/root/posix_rlimit/rlimit_data.bin"

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

/* ── RLIMIT_NOFILE: getrlimit returns sensible values ─────────────────────── */
static int test_rlimit_nofile_get(void)
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
		return fail_errno("getrlimit RLIMIT_NOFILE");

	if ((long long)rl.rlim_cur <= 0)
	{
		fprintf(stderr, "RLIMIT_NOFILE cur=%lld (expected > 0)\n",
			(long long)rl.rlim_cur);
		return 1;
	}
	if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < rl.rlim_cur)
	{
		fprintf(stderr, "RLIMIT_NOFILE max < cur\n");
		return 1;
	}
	return 0;
}

/* ── RLIMIT_NOFILE: setrlimit lower then restore ─────────────────────────── */
static int test_rlimit_nofile_set(void)
{
	struct rlimit old_rl, new_rl, check_rl;

	if (getrlimit(RLIMIT_NOFILE, &old_rl) != 0)
		return fail_errno("getrlimit before set");

	/* Lower the soft limit to 16 (above stdin/stdout/stderr). */
	new_rl.rlim_cur = 16;
	new_rl.rlim_max = old_rl.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &new_rl) != 0)
		return fail_errno("setrlimit to 16");

	if (getrlimit(RLIMIT_NOFILE, &check_rl) != 0)
		return fail_errno("getrlimit after set");
	if (check_rl.rlim_cur != 16)
	{
		fprintf(stderr, "RLIMIT_NOFILE after setrlimit: cur=%lld expected 16\n",
			(long long)check_rl.rlim_cur);
		return 1;
	}

	/* Restore original soft limit. */
	if (setrlimit(RLIMIT_NOFILE, &old_rl) != 0)
		return fail_errno("setrlimit restore");

	if (getrlimit(RLIMIT_NOFILE, &check_rl) != 0)
		return fail_errno("getrlimit after restore");
	if (check_rl.rlim_cur != old_rl.rlim_cur)
	{
		fprintf(stderr, "RLIMIT_NOFILE restore: cur=%lld expected %lld\n",
			(long long)check_rl.rlim_cur, (long long)old_rl.rlim_cur);
		return 1;
	}
	return 0;
}

/* ── RLIMIT_FSIZE: write beyond limit triggers SIGXFSZ / EFBIG ───────────── */
static int test_rlimit_fsize(void)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return fail_errno("fork rlimit_fsize");

	if (pid == 0)
	{
		struct rlimit rl;
		int fd;
		char buf[512];
		ssize_t n;

		/* Limit file size to 256 bytes. */
		rl.rlim_cur = 256;
		rl.rlim_max = 256;
		if (setrlimit(RLIMIT_FSIZE, &rl) != 0) _exit(1);

		unlink(TMPFILE);
		fd = open(TMPFILE, O_CREAT | O_WRONLY | O_TRUNC, 0600);
		if (fd < 0) _exit(2);

		/* Write 256 bytes — should succeed. */
		memset(buf, 0x5a, 256);
		n = write(fd, buf, 256);
		if (n != 256) { close(fd); _exit(3); }

		/* Write 1 more byte past the limit — should fail. */
		n = write(fd, buf, 1);
		if (n != -1 || errno != EFBIG)
		{
			fprintf(stderr, "write past RLIMIT_FSIZE: n=%ld errno=%d (expected -1/EFBIG)\n",
				(long)n, errno);
			close(fd);
			_exit(4);
		}

		close(fd);
		_exit(0);
	}

	if (waitpid(pid, &status, 0) < 0)
		return fail_errno("waitpid rlimit_fsize");

	if (!WIFEXITED(status))
	{
		/* Killed by SIGXFSZ is also acceptable. */
		if (WIFSIGNALED(status))
			return 0;
		return fail("rlimit_fsize child did not exit normally");
	}
	if (WEXITSTATUS(status) != 0)
	{
		fprintf(stderr, "rlimit_fsize child exited with %d\n", WEXITSTATUS(status));
		return 1;
	}
	return 0;
}

/* ── RLIMIT_STACK: getrlimit returns sensible value ──────────────────────── */
static int test_rlimit_stack_get(void)
{
	struct rlimit rl;

	if (getrlimit(RLIMIT_STACK, &rl) != 0)
		return fail_errno("getrlimit RLIMIT_STACK");

	if ((long long)rl.rlim_cur <= 0 && rl.rlim_cur != RLIM_INFINITY)
	{
		fprintf(stderr, "RLIMIT_STACK cur=%lld (expected > 0 or RLIM_INFINITY)\n",
			(long long)rl.rlim_cur);
		return 1;
	}
	return 0;
}

int main(void)
{
	int rc = 0;

	if (test_rlimit_nofile_get() != 0) { fprintf(stderr, "FAIL: nofile_get\n");  rc = 1; }
	if (test_rlimit_nofile_set() != 0) { fprintf(stderr, "FAIL: nofile_set\n");  rc = 1; }
	if (test_rlimit_fsize()      != 0) { fprintf(stderr, "FAIL: fsize\n");       rc = 1; }
	if (test_rlimit_stack_get()  != 0) { fprintf(stderr, "FAIL: stack_get\n");   rc = 1; }

	if (rc == 0)
		printf("all rlimit tests passed\n");
	return rc;
}
EOF

expect_success gcc -o "$BIN" "$SRC"
[ -x "$BIN" ] || fail "compiled binary missing"
expect_success "$BIN"
