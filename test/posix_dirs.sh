#!/bin/sh

set -e
BASE=/root/posix_dirs
PARENT="$BASE/parent"
CHILD="$PARENT/child"

fail()
{
	echo "posix_dirs: $1" >&2
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

mkdir -p "$CHILD" >/dev/null 2>&1 || fail "mkdir -p failed"
cd "$CHILD" || fail "cd child failed"
pwd_now=$(pwd 2>/dev/null) || fail "pwd failed after cd child"
[ "$pwd_now" = "$CHILD" ] || fail "Expected: pwd '$CHILD'
Actual: '${pwd_now}'"

cd . || fail "cd . failed"
pwd_now=$(pwd 2>/dev/null) || fail "pwd failed after cd ."
[ "$pwd_now" = "$CHILD" ] || fail "Expected: pwd '$CHILD' after cd .
Actual: '${pwd_now}'"

cd .. || fail "cd .. failed"
pwd_now=$(pwd 2>/dev/null) || fail "pwd failed after cd .."
[ "$pwd_now" = "$PARENT" ] || fail "Expected: pwd '$PARENT' after cd ..
Actual: '${pwd_now}'"

printf 'x\n' > "$PARENT/file.txt"
expect_failure rmdir "$PARENT"

rm "$PARENT/file.txt" >/dev/null 2>&1 || fail "rm child file failed"
rmdir "$CHILD" >/dev/null 2>&1 || fail "rmdir empty child failed"
[ ! -e "$CHILD" ] || fail "child still exists after rmdir"
rmdir "$PARENT" >/dev/null 2>&1 || fail "rmdir empty parent failed"
[ ! -e "$PARENT" ] || fail "parent still exists after rmdir"
