#!/bin/sh

set -e

fail()
{
	echo "tty_basic: $1" >&2
	exit 1
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

require_cmd tty
require_cmd stty

tty_path=$(tty 2>/dev/null) || fail "tty command failed"
case "$tty_path" in
/dev/*)
	:
	;;
*)
	fail "Expected: tty path under /dev
Actual: '$tty_path'"
	;;
esac

expect_success_sh "stty -a >/dev/null"
expect_success_sh "stty -echo >/dev/null"
expect_success_sh "stty echo >/dev/null"
expect_success_sh "printf 'tty smoke\\n' > '$tty_path'"
