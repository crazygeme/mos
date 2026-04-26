#!/bin/sh

set -e
CASE_NAME=gcc_hello
BASE=/root/tests/$CASE_NAME
SRC="$BASE/hello.c"
BIN="$BASE/hello"
OUT="$BASE/out.txt"

fail()
{
	echo "gcc_hello: $1" >&2
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

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
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

require_cmd gcc

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf '%s\n' '#include <stdio.h>' > "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' 'int main(void)' >> "$SRC"
printf '%s\n' '{' >> "$SRC"
printf '%s\n' '	puts("hello, world");' >> "$SRC"
printf '%s\n' '	return 0;' >> "$SRC"
printf '%s\n' '}' >> "$SRC"

expect_success gcc -o "$BIN" "$SRC"
[ -x "$BIN" ] || fail "compiled binary missing"

set +e
"$BIN" > "$OUT" 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || fail "Expected: success from '$BIN'
Actual: exit status $rc
Output: $(cat "$OUT" 2>/dev/null || printf '<none>')"
output=$(cat "$OUT" 2>/dev/null) || fail "read output failed"
expect_eq "hello, world" "$output" "compiled program output"
