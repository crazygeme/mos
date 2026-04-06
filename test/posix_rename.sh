#!/bin/sh

set -e
BASE=/root/posix_rename
DIR="$BASE/dir"
SRC="$DIR/src.txt"
DST="$DIR/dst.txt"
SUB="$DIR/sub"

fail()
{
	echo "posix_rename: $1" >&2
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
printf 'src\n' > "$SRC"
printf 'dst\n' > "$DST"

mv "$SRC" "$DST" >/dev/null 2>&1 || fail "rename over existing file failed"
[ ! -e "$SRC" ] || fail "Expected: old source removed after rename
Actual: '$SRC' still exists"
data=$(cat "$DST" 2>/dev/null) || fail "cat destination after replace failed"
expect_eq "src" "$data" "destination contents after replace"

mkdir "$SUB" >/dev/null 2>&1 || fail "mkdir sub failed"
printf 'x\n' > "$DIR/file.txt"
expect_failure mv "$DIR/file.txt" "$SUB"
