#!/bin/sh

set -e
BASE=/root/posix_pthread
SRC="$BASE/pthread_test.c"
BIN="$BASE/pthread_test"

fail()
{
	echo "posix_pthread: $1" >&2
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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int counter = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
	int id = (int)(long)arg;
	int rc;

	rc = pthread_mutex_lock(&lock);
	if (rc != 0) {
		fprintf(stderr, "pthread_mutex_lock: %s\n", strerror(rc));
		return (void *)1;
	}
	counter += id;
	rc = pthread_mutex_unlock(&lock);
	if (rc != 0) {
		fprintf(stderr, "pthread_mutex_unlock: %s\n", strerror(rc));
		return (void *)2;
	}

	return (void *)(long)(id + 10);
}

int main(void)
{
	pthread_t th[2];
	void *ret0 = NULL;
	void *ret1 = NULL;
	int rc;

	rc = pthread_create(&th[0], NULL, worker, (void *)1);
	if (rc != 0) {
		fprintf(stderr, "pthread_create #0: %s\n", strerror(rc));
		return 1;
	}
	rc = pthread_create(&th[1], NULL, worker, (void *)2);
	if (rc != 0) {
		fprintf(stderr, "pthread_create #1: %s\n", strerror(rc));
		return 1;
	}

	rc = pthread_join(th[0], &ret0);
	if (rc != 0) {
		fprintf(stderr, "pthread_join #0: %s\n", strerror(rc));
		return 1;
	}
	rc = pthread_join(th[1], &ret1);
	if (rc != 0) {
		fprintf(stderr, "pthread_join #1: %s\n", strerror(rc));
		return 1;
	}

	if ((long)ret0 != 11 && (long)ret1 != 11) {
		fprintf(stderr, "missing thread return 11: ret0=%ld ret1=%ld\n",
			(long)ret0, (long)ret1);
		return 1;
	}
	if ((long)ret0 != 12 && (long)ret1 != 12) {
		fprintf(stderr, "missing thread return 12: ret0=%ld ret1=%ld\n",
			(long)ret0, (long)ret1);
		return 1;
	}
	if (counter != 3) {
		fprintf(stderr, "counter mismatch: %d\n", counter);
		return 1;
	}

	puts("pthread ok");
	return 0;
}
EOF

expect_success gcc -Wall -Werror -pthread -o "$BIN" "$SRC"
[ -x "$BIN" ] || fail "compiled binary missing"

output=$("$BIN") || fail "pthread helper exited non-zero"
[ "$output" = "pthread ok" ] || fail "Expected output 'pthread ok'
Actual: ${output:-<none>}"
