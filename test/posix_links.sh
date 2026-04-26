#!/bin/sh

set -e
CASE_NAME=posix_links
BASE=/root/tests/$CASE_NAME
DIR="$BASE/dir"
SRC="$DIR/src.txt"
SYM="$DIR/src.sym"
HARD="$DIR/src.hard"
RENAMED="$DIR/src.renamed"

fail()
{
	echo "posix_links: $1" >&2
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

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$DIR" >/dev/null 2>&1 || fail "mkdir failed"
printf 'payload\n' > "$SRC"

ln -s "$SRC" "$SYM" >/dev/null 2>&1 || fail "symlink create failed"
[ -L "$SYM" ] || fail "symlink not visible after creation"
sym_data=$(cat "$SYM" 2>/dev/null) || fail "cat through symlink failed"
[ "$sym_data" = "payload" ] || fail "Expected: symlink contents 'payload'
Actual: '${sym_data}'"

ln "$SRC" "$HARD" >/dev/null 2>&1 || fail "hard link create failed"
[ -f "$HARD" ] || fail "hard link missing after creation"
hard_data=$(cat "$HARD" 2>/dev/null) || fail "cat through hard link failed"
[ "$hard_data" = "payload" ] || fail "Expected: hard link contents 'payload'
Actual: '${hard_data}'"

mv "$SRC" "$RENAMED" >/dev/null 2>&1 || fail "rename failed"
[ ! -e "$SRC" ] || fail "old path still exists after rename"
[ -f "$RENAMED" ] || fail "renamed file missing"
renamed_data=$(cat "$RENAMED" 2>/dev/null) || fail "cat renamed file failed"
[ "$renamed_data" = "payload" ] || fail "Expected: renamed file contents 'payload'
Actual: '${renamed_data}'"
hard_data=$(cat "$HARD" 2>/dev/null) || fail "cat hard link after rename failed"
[ "$hard_data" = "payload" ] || fail "Expected: hard link contents 'payload' after rename
Actual: '${hard_data}'"
expect_failure cat "$SYM"

rm "$RENAMED" >/dev/null 2>&1 || fail "unlink renamed file failed"
[ ! -e "$RENAMED" ] || fail "unlinked file still exists"
[ -f "$HARD" ] || fail "hard link lost after unlink of renamed file"
hard_data=$(cat "$HARD" 2>/dev/null) || fail "cat hard link after unlink failed"
[ "$hard_data" = "payload" ] || fail "Expected: hard link contents 'payload' after unlink
Actual: '${hard_data}'"
expect_failure cat "$SYM"
