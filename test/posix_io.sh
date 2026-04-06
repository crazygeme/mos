#!/bin/sh

set -e
BASE=/root/posix_io
FILE="$BASE/file.txt"
DIR="$BASE/dir"

fail()
{
	echo "posix_io: $1" >&2
	exit 1
}

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
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

printf 'abc\n' > "$FILE"
data=$(cat "$FILE" 2>/dev/null) || fail "cat after create failed"
expect_eq "abc" "$data" "initial file contents"

printf 'z\n' > "$FILE"
data=$(cat "$FILE" 2>/dev/null) || fail "cat after truncate failed"
expect_eq "z" "$data" "contents after truncate write"

printf '1\n' >> "$FILE"
printf '2\n' >> "$FILE"
data=$(cat "$FILE" 2>/dev/null) || fail "cat after append failed"
expect_eq "z
1
2" "$data" "contents after append writes"

: > "$FILE"
[ ! -s "$FILE" ] || fail "Expected: zero-length file after ': > file'
Actual: file still non-empty"

expect_failure cat "$DIR"
expect_failure rm "$DIR"
