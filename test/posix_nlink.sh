#!/bin/sh

set -e
BASE=/root/posix_nlink
FILE="$BASE/file.txt"
LINK="$BASE/file.link"

fail()
{
	echo "posix_nlink: $1" >&2
	exit 1
}

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
}

inode_of()
{
	set -- $(ls -id "$1" 2>/dev/null) || fail "Expected: inode for '$1'
Actual: ls -i failed"
	printf '%s\n' "$1"
}

nlink_of()
{
	set -- $(ls -ldn "$1" 2>/dev/null) || fail "Expected: metadata for '$1'
Actual: ls -ldn failed"
	printf '%s\n' "$2"
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf 'payload\n' > "$FILE"
ln "$FILE" "$LINK" >/dev/null 2>&1 || fail "hard link create failed"

file_inode=$(inode_of "$FILE")
link_inode=$(inode_of "$LINK")
expect_eq "$file_inode" "$link_inode" "hard link inode equality"

file_nlink=$(nlink_of "$FILE")
link_nlink=$(nlink_of "$LINK")
expect_eq "2" "$file_nlink" "file nlink after hard link"
expect_eq "2" "$link_nlink" "link nlink after hard link"

rm "$FILE" >/dev/null 2>&1 || fail "unlink original failed"
[ ! -e "$FILE" ] || fail "Expected: original path removed
Actual: '$FILE' still exists"
[ -f "$LINK" ] || fail "Expected: hard link remains after unlink original
Actual: '$LINK' missing"

link_nlink=$(nlink_of "$LINK")
expect_eq "1" "$link_nlink" "remaining hard link count after unlink original"
link_data=$(cat "$LINK" 2>/dev/null) || fail "cat remaining hard link failed"
expect_eq "payload" "$link_data" "remaining hard link contents"
