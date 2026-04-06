#!/bin/sh

set -e
BASE=/root/posix_exec
FILE="$BASE/run.sh"

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

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

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

expect_success sh "$FILE"
