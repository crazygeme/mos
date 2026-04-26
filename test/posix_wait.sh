#!/bin/sh

set -e
CASE_NAME=posix_wait
BASE=/root/tests/$CASE_NAME

fail()
{
	echo "posix_wait: $1" >&2
	exit 1
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$BASE"

# Helper: run a subshell with given exit code, return its wait status
wait_status()
{
	(exit "$1") &
	set +e
	wait $!
	_rc=$?
	set -e
	printf '%d' "$_rc"
}

# 1. Exit code 0 preserved
rc=$(wait_status 0)
[ "$rc" -eq 0 ] || fail "Expected: exit status 0
Actual: $rc"

# 2. Exit code 1 preserved
rc=$(wait_status 1)
[ "$rc" -eq 1 ] || fail "Expected: exit status 1
Actual: $rc"

# 3. Exit code 42 preserved
rc=$(wait_status 42)
[ "$rc" -eq 42 ] || fail "Expected: exit status 42
Actual: $rc"

# 4. Exit code 127 preserved
rc=$(wait_status 127)
[ "$rc" -eq 127 ] || fail "Expected: exit status 127
Actual: $rc"

# 5. Exit code 255 preserved (max one-byte value)
rc=$(wait_status 255)
[ "$rc" -eq 255 ] || fail "Expected: exit status 255
Actual: $rc"

# 6. Wait for specific PID among multiple children
(exit 10) &
pid_a=$!
(exit 20) &
pid_b=$!

set +e
wait "$pid_a"
rc_a=$?
wait "$pid_b"
rc_b=$?
set -e

[ "$rc_a" -eq 10 ] || fail "Expected: child A exit status 10
Actual: $rc_a"
[ "$rc_b" -eq 20 ] || fail "Expected: child B exit status 20
Actual: $rc_b"

# 7. Wait for children that finish in reverse order
(sleep 0; exit 7) &
pid_slow=$!
(exit 3) &
pid_fast=$!

set +e
wait "$pid_slow"
rc_slow=$?
wait "$pid_fast"
rc_fast=$?
set -e

[ "$rc_slow" -eq 7 ] || fail "Expected: slow child exit status 7
Actual: $rc_slow"
[ "$rc_fast" -eq 3 ] || fail "Expected: fast child exit status 3
Actual: $rc_fast"

# 8. Writing output in child is visible after wait
outfile="$BASE/out.txt"
(printf 'child-wrote\n' > "$outfile") &
wait $!
[ -f "$outfile" ] || fail "Expected: child output file to exist after wait"
data=$(cat "$outfile")
[ "$data" = "child-wrote" ] || fail "Expected: child output 'child-wrote'
Actual: '$data'"

# 9. Children run concurrently (both can write before parent reads)
(printf 'a\n' > "$BASE/ca.txt") &
(printf 'b\n' > "$BASE/cb.txt") &
wait
[ -f "$BASE/ca.txt" ] || fail "Expected: child a output file after wait"
[ -f "$BASE/cb.txt" ] || fail "Expected: child b output file after wait"
