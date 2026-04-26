#!/bin/sh

set -e
CASE_NAME=posix_pipe
BASE=/root/tests/$CASE_NAME
FILE="$BASE/file.txt"

fail()
{
	echo "posix_pipe: $1" >&2
	exit 1
}

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf 'alpha\nbeta\n' > "$FILE"

count=$(cat "$FILE" | wc -l 2>/dev/null) || fail "pipeline cat|wc failed"
count=$(printf '%s\n' "$count" | tr -d ' ') || fail "trim wc output failed"
expect_eq "2" "$count" "line count through pipe"

first=$(cat "$FILE" | head -n 1 2>/dev/null) || fail "pipeline cat|head failed"
expect_eq "alpha" "$first" "first line through pipe"

joined=$(printf 'a\nb\n' | tr '\n' ':' 2>/dev/null) || fail "pipeline printf|tr failed"
expect_eq "a:b:" "$joined" "pipe transform output"
