#!/bin/sh

set -e
CASE_NAME=posix_proc
CASE_MTIME=@CASE_MTIME@
BASE=/root/tests/$CASE_NAME
WORKDIR=/root/tests/$CASE_NAME
SRC="$BASE/proc_test.c"
BIN="$BASE/proc_test"
STAMP="$BASE/.case_timestamp"

fail()
{
	echo "posix_proc: $1" >&2
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
	need_rebuild=0
else
	need_rebuild=1
fi

# ── Shell-level checks (no compilation needed) ────────────────────────────

# PID is a positive integer
pid=$$
[ "$pid" -gt 0 ] || fail "Expected: $$ (PID) to be positive
Actual: $pid"

# PPID differs from PID
ppid=$PPID
[ "$ppid" -gt 0 ] || fail "Expected: PPID to be positive
Actual: $ppid"
[ "$ppid" -ne "$pid" ] || fail "Expected: PPID != PID
Actual: both are $pid"

# Child PID differs from parent PID
child_pid=$(sh -c 'printf "%d" "$$"')
[ "$child_pid" -ne "$pid" ] || fail "Expected: child PID != parent PID
Actual: both $pid"

# ── Compiled C tests ──────────────────────────────────────────────────────

if [ "$need_rebuild" -eq 0 ]; then
cat > "$SRC" << 'EOF'
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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

/* ── getpid / getppid ────────────────────────────────────────────────────── */
static int test_getpid(void)
{
	pid_t pid  = getpid();
	pid_t ppid = getppid();

	if (pid <= 0)
	{
		fprintf(stderr, "getpid returned %d\n", (int)pid);
		return 1;
	}
	if (ppid <= 0)
	{
		fprintf(stderr, "getppid returned %d\n", (int)ppid);
		return 1;
	}
	if (pid == ppid)
		return fail("getpid == getppid (should be different)");

	/* Calling getpid twice must return the same value. */
	if (getpid() != pid)
		return fail("getpid returned different values on two calls");

	return 0;
}

/* ── fork: child inherits PID continuity ─────────────────────────────────── */
static int test_fork_getpid(void)
{
	pid_t ppid = getpid();
	pid_t child_pid;
	int status;

	child_pid = fork();
	if (child_pid < 0)
		return fail_errno("fork");

	if (child_pid == 0)
	{
		/* Child: my PID must differ from parent's PID. */
		if (getpid() == ppid)
		{
			fprintf(stderr, "child getpid() == parent getpid()\n");
			_exit(1);
		}
		/* Child: my PPID must equal parent's PID. */
		if (getppid() != ppid)
		{
			fprintf(stderr, "child getppid() %d != parent pid %d\n",
				(int)getppid(), (int)ppid);
			_exit(2);
		}
		_exit(0);
	}

	if (waitpid(child_pid, &status, 0) < 0)
		return fail_errno("waitpid fork_getpid");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		fprintf(stderr, "fork_getpid child failed with status %d\n",
			WEXITSTATUS(status));
		return 1;
	}
	return 0;
}

/* ── fork: exit codes 0, 1, 42 ──────────────────────────────────────────── */
static int test_fork_exit_code(int code)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return fail_errno("fork");
	if (pid == 0)
		_exit(code);

	if (waitpid(pid, &status, 0) < 0)
		return fail_errno("waitpid exit_code");
	if (!WIFEXITED(status))
		return fail("child not exited normally");
	if (WEXITSTATUS(status) != code)
	{
		fprintf(stderr, "exit code: expected %d, got %d\n",
			code, WEXITSTATUS(status));
		return 1;
	}
	return 0;
}

/* ── fork: multiple children ─────────────────────────────────────────────── */
static int test_fork_multiple(void)
{
	pid_t pids[4];
	int codes[4] = {0, 1, 42, 127};
	int i, status;

	for (i = 0; i < 4; i++)
	{
		pids[i] = fork();
		if (pids[i] < 0)
			return fail_errno("fork multiple");
		if (pids[i] == 0)
			_exit(codes[i]);
	}

	for (i = 0; i < 4; i++)
	{
		if (waitpid(pids[i], &status, 0) < 0)
			return fail_errno("waitpid multiple");
		if (!WIFEXITED(status) || WEXITSTATUS(status) != codes[i])
		{
			fprintf(stderr, "child %d: expected exit %d, got %d\n",
				i, codes[i], WEXITSTATUS(status));
			return 1;
		}
	}
	return 0;
}

/* ── setsid: child becomes session leader ────────────────────────────────── */
static int test_setsid(void)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return fail_errno("fork setsid");
	if (pid == 0)
	{
		pid_t sid = setsid();
		if (sid < 0)
		{
			fprintf(stderr, "setsid: %s\n", strerror(errno));
			_exit(1);
		}
		/* After setsid, SID should equal our PID. */
		if (sid != getpid())
		{
			fprintf(stderr, "setsid returned %d, expected %d\n",
				(int)sid, (int)getpid());
			_exit(2);
		}
		/* getsid(0) should also return our PID. */
		if (getsid(0) != getpid())
		{
			fprintf(stderr, "getsid(0) %d != getpid %d after setsid\n",
				(int)getsid(0), (int)getpid());
			_exit(3);
		}
		_exit(0);
	}

	if (waitpid(pid, &status, 0) < 0)
		return fail_errno("waitpid setsid");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		fprintf(stderr, "setsid child failed with status %d\n",
			WEXITSTATUS(status));
		return 1;
	}
	return 0;
}

/* ── setpgid / getpgid ───────────────────────────────────────────────────── */
static int test_setpgid(void)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return fail_errno("fork setpgid");
	if (pid == 0)
	{
		/* Move child into its own process group. */
		if (setpgid(0, 0) < 0)
		{
			fprintf(stderr, "setpgid(0,0): %s\n", strerror(errno));
			_exit(1);
		}
		if (getpgid(0) != getpid())
		{
			fprintf(stderr, "getpgid(0) %d != getpid %d after setpgid\n",
				(int)getpgid(0), (int)getpid());
			_exit(2);
		}
		_exit(0);
	}

	if (waitpid(pid, &status, 0) < 0)
		return fail_errno("waitpid setpgid");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		fprintf(stderr, "setpgid child failed with status %d\n",
			WEXITSTATUS(status));
		return 1;
	}
	return 0;
}

int main(void)
{
	int rc = 0;

	if (test_getpid()           != 0) { fprintf(stderr, "FAIL: getpid\n");          rc = 1; }
	if (test_fork_getpid()      != 0) { fprintf(stderr, "FAIL: fork_getpid\n");     rc = 1; }
	if (test_fork_exit_code(0)  != 0) { fprintf(stderr, "FAIL: fork_exit 0\n");     rc = 1; }
	if (test_fork_exit_code(1)  != 0) { fprintf(stderr, "FAIL: fork_exit 1\n");     rc = 1; }
	if (test_fork_exit_code(42) != 0) { fprintf(stderr, "FAIL: fork_exit 42\n");    rc = 1; }
	if (test_fork_multiple()    != 0) { fprintf(stderr, "FAIL: fork_multiple\n");   rc = 1; }
	if (test_setsid()           != 0) { fprintf(stderr, "FAIL: setsid\n");          rc = 1; }
	if (test_setpgid()          != 0) { fprintf(stderr, "FAIL: setpgid\n");         rc = 1; }

	if (rc == 0)
		printf("all proc tests passed\n");
	return rc;
}
EOF

expect_success gcc -o "$BIN" "$SRC"
update_case_timestamp
fi
[ -x "$BIN" ] || fail "compiled binary missing"
expect_success "$BIN"
