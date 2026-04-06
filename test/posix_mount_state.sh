#!/bin/sh

set -e
BASE=/root/posix_mount_state
IMG="$BASE/test.img"
MNT="$BASE/mnt"

fail()
{
	echo "posix_mount_state: $1" >&2
	exit 1
}

expect_contains()
{
	grep -F "$1" "$2" >/dev/null || fail "Expected: '$1' in $2
Actual: missing"
}

expect_not_contains()
{
	if grep -F "$1" "$2" >/dev/null; then
		fail "Expected: '$1' absent from $2
Actual: present"
	fi
}

mount_dev()
{
	while read -r dev mount rest; do
		[ "$mount" = "$MNT" ] || continue
		printf '%s\n' "$dev"
		return 0
	done < /proc/mounts

	return 1
}

cleanup()
{
	umount "$MNT" >/dev/null 2>&1 || true
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$MNT" >/dev/null 2>&1 || fail "mkdir failed"
dd if=/dev/zero of="$IMG" bs=1M count=8 >/dev/null 2>&1 || fail "dd failed"
mkfs.ext3 -F "$IMG" >/dev/null 2>&1 || fail "mkfs failed"

expect_not_contains "$MNT" /proc/mounts
mount -o loop "$IMG" "$MNT" >/dev/null 2>&1 || fail "mount failed"
LOOP_DEV=$(mount_dev) || fail "missing mount entry after mount"
expect_contains "$MNT" /proc/mounts
expect_contains "${LOOP_DEV#/dev/}" /proc/partitions

umount "$MNT" >/dev/null 2>&1 || fail "umount failed"
expect_not_contains "$MNT" /proc/mounts
