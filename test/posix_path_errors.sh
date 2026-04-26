#!/bin/sh

set -e
CASE_NAME=posix_path_errors
BASE=/root/tests/$CASE_NAME
DIR="$BASE/dir"
FILE="$DIR/file.txt"
MISSING="$BASE/missing"

fail()
{
	echo "posix_path_errors: $1" >&2
	exit 1
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

mkdir -p "$DIR" >/dev/null 2>&1 || fail "mkdir failed"
printf 'payload\n' > "$FILE"

expect_failure cat "$MISSING"
expect_failure cd "$MISSING"
expect_failure mkdir "$FILE/child"
expect_failure cat "$DIR/"
expect_failure rm "$DIR/"
