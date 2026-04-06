#!/bin/sh
# loopdev integration test - runs entirely inside MOS
# requires: dd, mkfs.ext3, mount, umount

set -e
IMG=/root/test.img
MNT=/root/mnt
FILE="$MNT/hello.txt"
DATA='loop_ok'

fail()
{
	echo "loopdev: $1" >&2
	exit 1
}

check_file()
{
	[ -f "$1" ] || fail "missing file: $1"
}

check_not_exists()
{
	[ ! -e "$1" ] || fail "unexpected path exists: $1"
}

check_contains()
{
	grep -F "$1" "$2" >/dev/null || fail "missing '$1' in $2"
}

check_not_contains()
{
	if grep -F "$1" "$2" >/dev/null; then
		fail "unexpected '$1' in $2"
	fi
}

check_equals()
{
	[ "$1" = "$2" ] || fail "$3"
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

check_loop_mounted()
{
	dev=$(mount_dev) || fail "missing mount entry for $MNT"
	case "$dev" in
	/dev/loop*)
		printf '%s\n' "$dev"
		;;
	*)
		fail "unexpected mount device for $MNT: $dev"
		;;
	esac
}

check_not_mounted()
{
	if mount_dev >/dev/null 2>&1; then
		fail "unexpected mount entry for $MNT in /proc/mounts"
	fi
}

cleanup()
{
	umount "$MNT" >/dev/null 2>&1 || true
	rm -rf "$IMG" "$MNT" >/dev/null 2>&1 || true
}

trap cleanup EXIT

cleanup

dd if=/dev/zero of="$IMG" bs=1M count=16 >/dev/null 2>&1
check_file "$IMG"
mkdir -p "$MNT"
check_file "$IMG"

mkfs.ext3 -F "$IMG" >/dev/null 2>&1
mount -o loop "$IMG" "$MNT" >/dev/null 2>&1
LOOP_DEV=$(check_loop_mounted)
LOOP_NAME=${LOOP_DEV#/dev/}
check_contains "$LOOP_NAME" /proc/partitions
check_not_exists "$FILE"

printf '%s\n' "$DATA" > "$FILE"
check_file "$FILE"
read_back=$(cat "$FILE")
check_equals "$read_back" "$DATA" "file contents changed after write"

sync >/dev/null 2>&1 || true
umount "$MNT" >/dev/null 2>&1
check_not_mounted
check_not_exists "$FILE"

mount -o loop "$IMG" "$MNT" >/dev/null 2>&1
LOOP_DEV=$(check_loop_mounted)
check_file "$FILE"
read_back=$(cat "$FILE")
check_equals "$read_back" "$DATA" "file contents not persisted after remount"

rm "$FILE"
check_not_exists "$FILE"
umount "$MNT" >/dev/null 2>&1
check_not_mounted
