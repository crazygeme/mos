#!/bin/sh

set -e
BASE=/root/posix_mkdir
DIR="$BASE/dir"
FILE="$BASE/file.txt"
CHILD="$BASE/missing/child"

fail()
{
	echo "posix_mkdir: $1" >&2
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

expect_failure()
{
	set +e
	output=$("$@" 2>&1)
	rc=$?
	set -e
	[ "$rc" -ne 0 ] && return 0
	fail "Expected: failure from '$*'
Actual: success
Output: ${output:-<none>}"
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

expect_success mkdir "$BASE"
expect_success mkdir "$DIR"
expect_failure mkdir "$DIR"

printf 'x\n' > "$FILE"
expect_failure mkdir "$FILE"
expect_failure mkdir "$CHILD"

expect_success rmdir "$DIR"
expect_failure rmdir "$DIR"
