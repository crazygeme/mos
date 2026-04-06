#!/bin/sh

set -e
BASE=/root/ret_fs
DIR="$BASE/dir"
FILE="$DIR/file.txt"

fail()
{
	echo "ret_fs: $1" >&2
	exit 1
}

expect_success()
{
	set +e
	output=$("$@" 2>&1)
	rc=$?
	set -e
	[ "$rc" -eq 0 ] && return 0
	fail "Expected: success from '$*'
Actual: exit status $rc
Output: ${output:-<none>}"
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

expect_failure cat "$FILE"
expect_failure cd "$BASE"

expect_success mkdir -p "$DIR"
expect_failure mkdir "$DIR"

printf 'hello\n' > "$FILE"
read_back=$(cat "$FILE" 2>/dev/null) || fail "cat existing file failed"
[ "$read_back" = "hello" ] || fail "Expected: file contents 'hello'
Actual: '${read_back}'"

cd "$DIR" || fail "cd existing dir failed"
cwd=$(pwd 2>/dev/null) || fail "pwd failed"
[ "$cwd" = "$DIR" ] || fail "Expected: pwd '$DIR'
Actual: '${cwd}'"

expect_failure cd "$FILE"

rm "$FILE" >/dev/null 2>&1 || fail "rm existing file failed"
expect_failure cat "$FILE"
