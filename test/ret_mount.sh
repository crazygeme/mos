#!/bin/sh

set -e
CASE_NAME=ret_mount
BASE=/root/tests/$CASE_NAME
IMG="$BASE/test.img"
MNT="$BASE/mnt"

fail()
{
	echo "ret_mount: $1" >&2
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
	umount "$MNT" >/dev/null 2>&1 || true
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

expect_success mkdir -p "$MNT"
expect_failure mount -o loop "$IMG" "$MNT"
expect_failure umount "$MNT"

dd if=/dev/zero of="$IMG" bs=1M count=8 >/dev/null 2>&1 || fail "dd failed"
expect_success mkfs.ext3 -F "$IMG"
expect_success mount -o loop "$IMG" "$MNT"

expect_failure mount -o loop "$IMG" "$MNT"
expect_success umount "$MNT"
expect_failure umount "$MNT"
