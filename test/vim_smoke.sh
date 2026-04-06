#!/bin/sh

set -e
BASE=/root/vim_smoke
FILE="$BASE/file.txt"

fail()
{
	echo "vim_smoke: $1" >&2
	exit 1
}

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	detail=
	if [ -e "$FILE" ]; then
		size=$(wc -c < "$FILE" 2>/dev/null)
		visible=$(sed -n l "$FILE" 2>/dev/null)
		[ -n "$visible" ] || visible="<empty>"
		detail="
File size: ${size:-unknown}
File contents: $visible"
	fi
	fail "Expected: $3 = '$1'
Actual: '$2'$detail"
}

expect_success_sh()
{
	set +e
	output=$(sh -c "$1" 2>&1)
	rc=$?
	set -e
	[ "$rc" -eq 0 ] && return 0
	fail "Expected: success from '$1'
Actual: exit status $rc
Output: ${output:-<none>}"
}

require_cmd()
{
	command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

require_cmd vim

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf 'old\n' > "$FILE"

expect_success_sh "vim -u NONE -n -es -c 'set nomore' -c 'qall!' '$FILE'"
output=$(cat "$FILE" 2>/dev/null) || fail "read file after first vim run failed"
expect_eq "old" "$output" "file contents after open-and-quit vim run"

expect_success_sh "vim -u NONE -n -es -c 'set nomore' -c 'wq!' '$FILE'"
output=$(cat "$FILE" 2>/dev/null) || fail "read file after second vim run failed"
expect_eq "old" "$output" "file contents after open-and-write vim run"
