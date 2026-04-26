#!/bin/sh

set -e
CASE_NAME=emacs_smoke
BASE=/root/tests/$CASE_NAME
FILE="$BASE/file.txt"

fail()
{
	echo "emacs_smoke: $1" >&2
	exit 1
}

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
}

expect_success_sh()
{
	set +e
	output=$(sh -c "$1" 2>&1)
	rc=$?
	set -e
	[ "$rc" -eq 0 ] && return 0
	fail "Expected: success from '$1'
Actual: exit status $rc
Output: ${output:-<none>}"
}

require_cmd()
{
	command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

ensure_local_hostname()
{
	nodename=$(uname -n 2>/dev/null)
	[ -n "$nodename" ] || return 0

	if grep -Eq "(^|[[:space:]])$nodename([[:space:]]|\$)" /etc/hosts 2>/dev/null; then
		return 0
	fi

	printf '127.0.0.1 localhost %s\n' "$nodename" >> /etc/hosts 2>/dev/null ||
		fail "failed to seed /etc/hosts for $nodename"
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

require_cmd emacs
ensure_local_hostname

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf 'old\n' > "$FILE"

expect_success_sh "emacs -batch -q '$FILE' --eval '(progn (erase-buffer) (insert \"hello from emacs\\n\") (save-buffer))'"
output=$(cat "$FILE" 2>/dev/null) || fail "read file after first emacs run failed"
expect_eq "hello from emacs" "$output" "file contents after first emacs run"

expect_success_sh "emacs -batch -q '$FILE' --eval '(progn (goto-char (point-min)) (search-forward \"emacs\") (replace-match \"Emacs\") (save-buffer))'"
output=$(cat "$FILE" 2>/dev/null) || fail "read file after second emacs run failed"
expect_eq "hello from Emacs" "$output" "file contents after replacement"
