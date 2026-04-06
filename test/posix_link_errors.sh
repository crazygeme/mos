#!/bin/sh

set -e
BASE=/root/posix_link_errors
DIR="$BASE/dir"
FILE="$BASE/file.txt"
LINK="$BASE/file.link"
MISSING="$BASE/missing.txt"

fail()
{
	echo "posix_link_errors: $1" >&2
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

expect_failure ln "$MISSING" "$LINK"
expect_failure ln "$DIR" "$LINK"

ln "$FILE" "$LINK" >/dev/null 2>&1 || fail "hard link create failed"
expect_failure ln "$FILE" "$LINK"
