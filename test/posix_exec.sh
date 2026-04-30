#!/bin/sh

set -e
CASE_NAME=posix_exec
BASE=/root/tests/$CASE_NAME
FILE="$BASE/run.sh"
SRC="$BASE/exec_script.c"
BIN="$BASE/exec_script"

fail()
{
	echo "posix_exec: $1" >&2
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
printf '#!/bin/sh\nexit 7\n' > "$FILE"

chmod 0644 "$FILE" >/dev/null 2>&1 || fail "chmod 0644 failed"
expect_failure "$FILE"

chmod 0755 "$FILE" >/dev/null 2>&1 || fail "chmod 0755 failed"
set +e
"$FILE" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 7 ] || fail "Expected: script exit status 7
Actual: ${rc}"

set +e
sh "$FILE" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 7 ] || fail "Expected: 'sh $FILE' exit status 7
Actual: ${rc}"

printf '#!/bin/sh\n[ "$0" = "%s" ] || exit 31\nexit 23\n' "$FILE" > "$FILE"
cat > "$SRC" <<EOF
#include <unistd.h>

extern char **environ;

int main(void)
{
	char *argv[] = { "not-the-script-path", "arg1", 0 };
	execve("$FILE", argv, environ);
	return 99;
}
EOF
expect_success gcc "$SRC" -o "$BIN"
set +e
"$BIN" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 23 ] || fail "Expected: shebang passes script path as argv[1]
Actual: ${rc}"
