#!/bin/sh

set -e
CASE_NAME=posix_fd
BASE=/root/tests/$CASE_NAME
FILE="$BASE/file.txt"

fail()
{
	echo "posix_fd: $1" >&2
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
	exec 3<&- 4<&- 5>&- 6>&- >/dev/null 2>&1 || true
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf 'line1\nline2\n' > "$FILE"

exec 3<"$FILE" || fail "open fd 3 for read failed"
line=$(IFS= read -r x <&3; printf '%s' "$x") || fail "read first line from fd 3 failed"
expect_eq "line1" "$line" "first line via fd 3"
line=$(IFS= read -r x <&3; printf '%s' "$x") || fail "read second line from fd 3 failed"
expect_eq "line2" "$line" "second line via fd 3"
set +e
IFS= read -r line <&3
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "Expected: EOF from fd 3 after two reads
Actual: read returned success"
exec 3<&-

exec 4<"$FILE" || fail "open fd 4 for read failed"
exec 5<&4 || fail "dup fd 4 into fd 5 failed"
line=$(IFS= read -r x <&4; printf '%s' "$x") || fail "read from fd 4 failed"
expect_eq "line1" "$line" "first line via fd 4"
line=$(IFS= read -r x <&5; printf '%s' "$x") || fail "read from dup fd 5 failed"
expect_eq "line2" "$line" "second line via dup fd 5"
exec 4<&-
exec 5<&-

exec 6>"$BASE/out.txt" || fail "open fd 6 for write failed"
printf 'abc\n' >&6 || fail "write through fd 6 failed"
exec 6>&-
data=$(cat "$BASE/out.txt" 2>/dev/null) || fail "cat out.txt failed"
expect_eq "abc" "$data" "contents written through fd 6"
