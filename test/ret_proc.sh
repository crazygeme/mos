#!/bin/sh

set -e

fail()
{
	echo "ret_proc: $1" >&2
	exit 1
}

set +e
( exit 7 )
rc=$?
set -e
[ "$rc" -eq 7 ] || fail "Expected: subshell exit status 7
Actual: ${rc}"

( exit 0 )
rc=$?
[ "$rc" -eq 0 ] || fail "Expected: subshell exit status 0
Actual: ${rc}"

set +e
( exit 9 ) &
pid=$!
wait "$pid"
rc=$?
set -e
[ "$rc" -eq 9 ] || fail "Expected: wait status 9
Actual: ${rc}"

if ( exit 0 ); then
	:
else
	fail "if-condition treated exit 0 as failure"
fi

if ( exit 3 ); then
	fail "if-condition treated exit 3 as success"
fi
