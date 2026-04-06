#!/bin/sh

set -e
BASE=/root/posix_unlink
DIR="$BASE/dir"
TARGET="$DIR/target.txt"
SYM="$DIR/target.sym"

fail()
{
	echo "posix_unlink: $1" >&2
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
printf 'payload\n' > "$TARGET"
ln -s "$TARGET" "$SYM" >/dev/null 2>&1 || fail "ln -s failed"

rm "$SYM" >/dev/null 2>&1 || fail "rm symlink failed"
[ ! -e "$SYM" ] || fail "Expected: symlink path removed
Actual: '$SYM' still exists"
[ -f "$TARGET" ] || fail "Expected: symlink target preserved after unlink link
Actual: '$TARGET' missing"

expect_failure rmdir "$TARGET"
expect_failure rm "$DIR"

rm "$TARGET" >/dev/null 2>&1 || fail "rm regular file failed"
[ ! -e "$TARGET" ] || fail "Expected: regular file removed
Actual: '$TARGET' still exists"
