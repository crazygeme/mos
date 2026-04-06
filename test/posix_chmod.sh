#!/bin/sh

set -e
BASE=/root/posix_chmod
FILE="$BASE/file.sh"

fail()
{
	echo "posix_chmod: $1" >&2
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

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf '#!/bin/sh\nexit 0\n' > "$FILE"

[ -f "$FILE" ] || fail "file missing after create"

chmod 0644 "$FILE" >/dev/null 2>&1 || fail "chmod 0644 failed"
[ ! -x "$FILE" ] || fail "file unexpectedly executable after chmod 0644"
expect_failure "$FILE"

chmod 0755 "$FILE" >/dev/null 2>&1 || fail "chmod 0755 failed"
[ -x "$FILE" ] || fail "file not executable after chmod 0755"
expect_success "$FILE"

chmod 0000 "$FILE" >/dev/null 2>&1 || fail "chmod 0000 failed"
[ ! -x "$FILE" ] || fail "file still executable after chmod 0000"
expect_failure "$FILE"
