#!/bin/sh

set -e
BASE=/root/posix_path
DIR="$BASE/dir"
SUB="$DIR/sub"
FILE="$DIR/file.txt"

fail()
{
	echo "posix_path: $1" >&2
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

mkdir -p "$SUB" >/dev/null 2>&1 || fail "mkdir -p failed"
printf 'x\n' > "$FILE"

cd "$SUB" || fail "cd sub failed"
[ "$(pwd 2>/dev/null)" = "$SUB" ] || fail "Expected: pwd '$SUB'
Actual: '$(pwd 2>/dev/null)'"

cd ../. || fail "cd ../. failed"
[ "$(pwd 2>/dev/null)" = "$DIR" ] || fail "Expected: pwd '$DIR' after cd ../.
Actual: '$(pwd 2>/dev/null)'"

cd "$BASE/dir/../dir/sub/.." || fail "cd normalized path failed"
[ "$(pwd 2>/dev/null)" = "$DIR" ] || fail "Expected: pwd '$DIR' after normalized cd
Actual: '$(pwd 2>/dev/null)'"

expect_failure cd "$FILE"
expect_failure mkdir "$FILE/child"
