#!/bin/sh

set -e
CASE_NAME=posix_signal
BASE=/root/tests/$CASE_NAME

fail()
{
	echo "posix_signal: $1" >&2
	exit 1
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$BASE"

# 1. SIGTERM delivered to self via trap
caught=''
trap 'caught=yes' TERM
sh -c 'kill -TERM "$PPID"'
trap - TERM
[ "$caught" = yes ] || fail "SIGTERM not delivered to self"

# 2. SIGINT delivered to self via trap
caught=''
trap 'caught=yes' INT
sh -c 'kill -INT "$PPID"'
trap - INT
[ "$caught" = yes ] || fail "SIGINT not delivered to self"

# 3. SIGUSR1 delivered to self via trap
caught=''
trap 'caught=usr1' USR1
sh -c 'kill -USR1 "$PPID"'
trap - USR1
[ "$caught" = usr1 ] || fail "SIGUSR1 not delivered to self"

# 4. SIGUSR2 delivered to self via trap
caught=''
trap 'caught=usr2' USR2
sh -c 'kill -USR2 "$PPID"'
trap - USR2
[ "$caught" = usr2 ] || fail "SIGUSR2 not delivered to self"

# 5. kill -0 (existence check) succeeds for own PID
sh -c 'kill -0 "$PPID"' || fail "kill -0 on own PID failed"

# 6. SIGTERM kills a background child that does not trap it;
#    bash encodes signal-killed exit as 128+signum (SIGTERM=15 → 143)
(trap - TERM; sleep 30) &
child=$!
kill -TERM $child 2>/dev/null || true
set +e
wait $child
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "Expected: non-zero exit from SIGTERM-killed child
Actual: $rc"
[ "$rc" -ge 128 ] || fail "Expected: signal-killed exit status >= 128
Actual: $rc"

# 7. SIGKILL kills a background child unconditionally (128 + 9 = 137)
(sleep 30) &
child=$!
kill -KILL $child 2>/dev/null || true
set +e
wait $child
rc=$?
set -e
[ "$rc" -eq 137 ] || fail "Expected: SIGKILL exit status 137 (128+9)
Actual: $rc"

# 8. A trapped signal does not kill the process; it keeps running
result=$(
	trap 'printf caught' TERM
	sh -c 'kill -TERM "$PPID"'
	printf ' alive'
)
[ "$result" = "caught alive" ] || fail "Expected: 'caught alive' after trapped SIGTERM
Actual: '$result'"

# 9. Resetting a trap to default restores kill behaviour
(
	trap 'exit 0' TERM    # first override: trap TERM to exit cleanly
	trap - TERM            # reset to default: SIGTERM kills
	sleep 30
) &
child=$!
kill -TERM $child 2>/dev/null || true
set +e
wait $child
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "Expected: non-zero exit after trap reset to default
Actual: $rc"
