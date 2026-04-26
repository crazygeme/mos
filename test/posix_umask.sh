#!/bin/sh

set -e
CASE_NAME=posix_umask
BASE=/root/tests/$CASE_NAME

fail()
{
	echo "posix_umask: $1" >&2
	exit 1
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$BASE"

# Helper: extract permission string from ls -ldn output
mode_of()
{
	set -- $(ls -ldn "$1" 2>/dev/null) || fail "ls failed for $1"
	printf '%s' "$1"
}

# All file/dir creation is done in subshells so the script's own umask
# is not affected between cases.

# 1. umask 022 → file 0644 (-rw-r--r--), dir 0755 (drwxr-xr-x)
(
	umask 022
	: > "$BASE/file022"
	mkdir "$BASE/dir022"
)
m=$(mode_of "$BASE/file022")
[ "$m" = "-rw-r--r--" ] || fail "file022 mode with umask 022: expected -rw-r--r--
Actual: $m"
m=$(mode_of "$BASE/dir022")
[ "$m" = "drwxr-xr-x" ] || fail "dir022 mode with umask 022: expected drwxr-xr-x
Actual: $m"

# 2. umask 077 → file 0600 (-rw-------), dir 0700 (drwx------)
(
	umask 077
	: > "$BASE/file077"
	mkdir "$BASE/dir077"
)
m=$(mode_of "$BASE/file077")
[ "$m" = "-rw-------" ] || fail "file077 mode with umask 077: expected -rw-------
Actual: $m"
m=$(mode_of "$BASE/dir077")
[ "$m" = "drwx------" ] || fail "dir077 mode with umask 077: expected drwx------
Actual: $m"

# 3. umask 000 → file 0666 (-rw-rw-rw-), dir 0777 (drwxrwxrwx)
(
	umask 000
	: > "$BASE/file000"
	mkdir "$BASE/dir000"
)
m=$(mode_of "$BASE/file000")
[ "$m" = "-rw-rw-rw-" ] || fail "file000 mode with umask 000: expected -rw-rw-rw-
Actual: $m"
m=$(mode_of "$BASE/dir000")
[ "$m" = "drwxrwxrwx" ] || fail "dir000 mode with umask 000: expected drwxrwxrwx
Actual: $m"

# 4. umask 027 → file 0640 (-rw-r-----), dir 0750 (drwxr-x---)
(
	umask 027
	: > "$BASE/file027"
	mkdir "$BASE/dir027"
)
m=$(mode_of "$BASE/file027")
[ "$m" = "-rw-r-----" ] || fail "file027 mode with umask 027: expected -rw-r-----
Actual: $m"
m=$(mode_of "$BASE/dir027")
[ "$m" = "drwxr-x---" ] || fail "dir027 mode with umask 027: expected drwxr-x---
Actual: $m"

# 5. umask is per-process: parent's umask is unchanged after child subshell
orig_umask=$(umask)
(umask 000; : > "$BASE/nosideeffect")
current_umask=$(umask)
[ "$current_umask" = "$orig_umask" ] || fail "Expected: parent umask unchanged after subshell
Before: $orig_umask  After: $current_umask"

# 6. chmod can override the restricted mode set by umask
(
	umask 077
	: > "$BASE/chmod_test"
)
m=$(mode_of "$BASE/chmod_test")
[ "$m" = "-rw-------" ] || fail "chmod_test initial mode unexpected: $m"
chmod 0644 "$BASE/chmod_test"
m=$(mode_of "$BASE/chmod_test")
[ "$m" = "-rw-r--r--" ] || fail "chmod_test after chmod 0644: expected -rw-r--r--
Actual: $m"
