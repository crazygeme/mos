#!/bin/sh

set -e
BASE=/root/posix_futex
SRC="$BASE/futex_test.c"
BIN="$BASE/futex_test"

fail()
{
	echo "posix_futex: $1" >&2
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

cat > "$SRC" <<'EOF'
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile int fut = 0;

static int futex_wait(volatile int *uaddr, int val)
{
	struct timespec timeout;

	timeout.tv_sec = 1;
	timeout.tv_nsec = 0;
	return syscall(__NR_futex, (int *)uaddr, FUTEX_WAIT, val, &timeout, NULL,
		       0);
}

static int futex_wake(volatile int *uaddr, int nr)
{
	return syscall(__NR_futex, (int *)uaddr, FUTEX_WAKE, nr, NULL, NULL, 0);
}

static void *waiter(void *arg)
{
	int rc;

	(void)arg;
	rc = futex_wait(&fut, 0);
	if (rc != 0) {
		fprintf(stderr, "futex_wait failed: rc=%d errno=%d (%s)\n",
			rc, errno, strerror(errno));
		return (void *)1;
	}
	if (fut != 1) {
		fprintf(stderr, "waiter saw fut=%d after wake\n", fut);
		return (void *)2;
	}
	return 0;
}

int main(void)
{
	pthread_t th;
	void *ret = NULL;
	int rc;

	errno = 0;
	rc = futex_wait(&fut, 1);
	if (rc != -1 || errno != EAGAIN) {
		fprintf(stderr, "expected initial EAGAIN, rc=%d errno=%d\n",
			rc, errno);
		return 1;
	}

	rc = pthread_create(&th, NULL, waiter, NULL);
	if (rc != 0) {
		fprintf(stderr, "pthread_create: %s\n", strerror(rc));
		return 1;
	}

	sleep(1);
	fut = 1;
	rc = futex_wake(&fut, 1);
	if (rc != 1) {
		fprintf(stderr, "futex_wake returned %d\n", rc);
		return 1;
	}

	rc = pthread_join(th, &ret);
	if (rc != 0) {
		fprintf(stderr, "pthread_join: %s\n", strerror(rc));
		return 1;
	}
	if (ret != 0) {
		fprintf(stderr, "waiter returned %ld\n", (long)ret);
		return 1;
	}

	puts("futex ok");
	return 0;
}
EOF

expect_success gcc -Wall -Werror -pthread -o "$BIN" "$SRC"
[ -x "$BIN" ] || fail "compiled binary missing"

output=$("$BIN") || fail "futex helper exited non-zero"
[ "$output" = "futex ok" ] || fail "Expected output 'futex ok'
Actual: ${output:-<none>}"
