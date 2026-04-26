#!/bin/sh

set -e
CASE_NAME=posix_stat
BASE=/root/tests/$CASE_NAME
DIR="$BASE/dir"
FILE="$DIR/file.txt"
EMPTY="$DIR/empty.txt"
LINK="$DIR/file.link"

fail()
{
	echo "posix_stat: $1" >&2
	exit 1
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$DIR" >/dev/null 2>&1 || fail "mkdir failed"
printf 'abc\n' > "$FILE"
: > "$EMPTY"
ln -s "$FILE" "$LINK" >/dev/null 2>&1 || fail "ln -s failed"

[ -d "$DIR" ] || fail "test -d failed for directory"
[ ! -f "$DIR" ] || fail "directory reported as regular file"
[ -f "$FILE" ] || fail "test -f failed for regular file"
[ -s "$FILE" ] || fail "test -s failed for non-empty file"
[ ! -s "$EMPTY" ] || fail "empty file reported as non-empty"
[ -L "$LINK" ] || fail "test -L failed for symlink"
[ -e "$LINK" ] || fail "test -e failed for symlink target"
[ -f "$LINK" ] || fail "symlink to regular file not treated as regular file"
[ ! -d "$LINK" ] || fail "symlink to file reported as directory"
[ -x "$DIR" ] || fail "directory missing search permission"
