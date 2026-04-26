#!/bin/sh

set -e
CASE_NAME=posix_environ
BASE=/root/tests/$CASE_NAME

fail()
{
	echo "posix_environ: $1" >&2
	exit 1
}

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
}

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

mkdir -p "$BASE"

# 1. Exported variable is visible in child
MY_POSIX_ENV_VAR=hello123
export MY_POSIX_ENV_VAR
result=$(sh -c 'printf "%s" "$MY_POSIX_ENV_VAR"')
expect_eq "hello123" "$result" "exported variable in child"

# 2. Non-exported variable is not visible in child
POSIX_LOCAL_ONLY=secret
result=$(sh -c 'printf "%s" "${POSIX_LOCAL_ONLY:-notset}"')
expect_eq "notset" "$result" "non-exported variable invisible in child"

# 3. Updated value of exported variable is seen by child
MY_POSIX_ENV_VAR=updated
result=$(sh -c 'printf "%s" "$MY_POSIX_ENV_VAR"')
expect_eq "updated" "$result" "updated exported variable in child"

# 4. unset removes the variable from child environment
MY_POSIX_ENV_VAR=tobecleared
export MY_POSIX_ENV_VAR
unset MY_POSIX_ENV_VAR
result=$(sh -c 'printf "%s" "${MY_POSIX_ENV_VAR:-gone}"')
expect_eq "gone" "$result" "unset variable invisible in child"

# 5. HOME is set and non-empty
[ -n "$HOME" ] || fail "HOME is not set"

# 6. PATH is set and non-empty
[ -n "$PATH" ] || fail "PATH is not set"

# 7. Inline env-var assignment overrides for child only
MY_POSIX_INLINE=parent
export MY_POSIX_INLINE
result=$(MY_POSIX_INLINE=child sh -c 'printf "%s" "$MY_POSIX_INLINE"')
expect_eq "child" "$result" "inline env override in child"
# Parent value must be unchanged
expect_eq "parent" "$MY_POSIX_INLINE" "parent var unchanged after inline override"

# 8. env command produces output listing at least PATH
env_out=$(env 2>/dev/null) || fail "env command failed"
printf '%s\n' "$env_out" | grep -q '^PATH=' || fail "env output does not contain PATH="

# 9. Child's modification does not affect parent
SHARED_VAR=original
export SHARED_VAR
sh -c 'SHARED_VAR=modified; export SHARED_VAR'
expect_eq "original" "$SHARED_VAR" "child modification did not affect parent"

# 10. Variables with special characters in value
SPECIAL_VAR='hello world & "quotes" = value'
export SPECIAL_VAR
result=$(sh -c 'printf "%s" "$SPECIAL_VAR"')
expect_eq "$SPECIAL_VAR" "$result" "variable with special characters"

# 11. Empty-string export
EMPTY_EXPORT=
export EMPTY_EXPORT
result=$(sh -c 'printf "%s" "$EMPTY_EXPORT"')
expect_eq "" "$result" "empty exported variable"
