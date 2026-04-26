#!/bin/sh

set -e
CASE_NAME=posix_readlink
BASE=/root/tests/$CASE_NAME
DIR="$BASE/dir"
TARGET="$DIR/target.txt"
SYM="$DIR/target.sym"

fail()
{
	echo "posix_readlink: $1" >&2
	exit 1
}

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
}

require_cmd()
{
	command -v "$1" >/dev/null 2>&1 || fail "Expected: command '$1' available
Actual: missing"
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

require_cmd readlink

mkdir -p "$DIR" >/dev/null 2>&1 || fail "mkdir failed"
printf 'x\n' > "$TARGET"
ln -s "$TARGET" "$SYM" >/dev/null 2>&1 || fail "ln -s failed"

target_path=$(readlink "$SYM" 2>/dev/null) || fail "readlink failed"
expect_eq "$TARGET" "$target_path" "readlink target"

mv "$TARGET" "$DIR/renamed.txt" >/dev/null 2>&1 || fail "rename target failed"
target_path=$(readlink "$SYM" 2>/dev/null) || fail "readlink after rename failed"
expect_eq "$TARGET" "$target_path" "readlink target after rename"
