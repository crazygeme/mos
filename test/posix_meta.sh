#!/bin/sh

set -e
CASE_NAME=posix_meta
BASE=/root/tests/$CASE_NAME
DIR="$BASE/dir"
CHILD="$DIR/child"
FILE="$DIR/file.txt"

fail()
{
	echo "posix_meta: $1" >&2
	exit 1
}

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
}

require_cmd()
{
	command -v "$1" >/dev/null 2>&1 || fail "Expected: command '$1' available
Actual: missing"
}

meta_of()
{
	set -- $(ls -ldn "$1" 2>/dev/null) || fail "Expected: ls metadata for '$1'
Actual: ls failed"
	META_MODE=$1
	META_NLINK=$2
	META_UID=$3
	META_GID=$4
	META_SIZE=$5
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

require_cmd ls
require_cmd chown

mkdir -p "$DIR" >/dev/null 2>&1 || fail "mkdir failed"
printf 'abc\n' > "$FILE"
chmod 0640 "$FILE" >/dev/null 2>&1 || fail "chmod 0640 failed"
chown 123:456 "$FILE" >/dev/null 2>&1 || fail "chown 123:456 failed"

meta_of "$FILE"
expect_eq "-rw-r-----" "$META_MODE" "file mode"
expect_eq "1" "$META_NLINK" "file link count"
expect_eq "123" "$META_UID" "file uid"
expect_eq "456" "$META_GID" "file gid"
expect_eq "4" "$META_SIZE" "file size"

meta_of "$DIR"
case "$META_MODE" in
drwxr-xr-x|drwx------|drwxrwxr-x|drwxrwx---|drwxrwxrwx)
	:
	;;
*)
	fail "Expected: directory mode starting with 'd'
Actual: '$META_MODE'"
	;;
esac
expect_eq "2" "$META_NLINK" "empty directory link count"

mkdir "$CHILD" >/dev/null 2>&1 || fail "mkdir child failed"
meta_of "$DIR"
expect_eq "3" "$META_NLINK" "parent directory link count after subdir create"

rmdir "$CHILD" >/dev/null 2>&1 || fail "rmdir child failed"
meta_of "$DIR"
expect_eq "2" "$META_NLINK" "parent directory link count after subdir remove"
