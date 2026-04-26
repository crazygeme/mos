#!/bin/sh

set -e
CASE_NAME=posix_symlink
BASE=/root/tests/$CASE_NAME
DIR="$BASE/dir"
TARGET="$DIR/target.txt"
SYM="$DIR/link.txt"

fail()
{
	echo "posix_symlink: $1" >&2
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

[ -L "$SYM" ] || fail "Expected: symlink path
Actual: test -L failed"
[ -e "$SYM" ] || fail "Expected: existing symlink target
Actual: test -e failed"

rm "$TARGET" >/dev/null 2>&1 || fail "rm target failed"
[ -L "$SYM" ] || fail "Expected: dangling symlink still reported by test -L
Actual: test -L failed"
[ ! -e "$SYM" ] || fail "Expected: dangling symlink to fail test -e
Actual: test -e succeeded"
expect_failure cat "$SYM"

printf 'new\n' > "$TARGET"
[ -e "$SYM" ] || fail "Expected: recreated target makes symlink exist again
Actual: test -e failed"
[ "$(cat "$SYM" 2>/dev/null)" = "new" ] || fail "Expected: symlink contents 'new'
Actual: '$(cat "$SYM" 2>/dev/null)'"
