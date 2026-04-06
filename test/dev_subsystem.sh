#!/bin/sh

set -e
BASE=/root/dev_subsystem
SRC="$BASE/dev_probe.c"
BIN="$BASE/dev_probe"

fail()
{
	echo "dev_subsystem: $1" >&2
	exit 1
}

expect_success()
{
	set +e
	output=$("$@" 2>&1)
	rc=$?
	set -e
	[ "$rc" -eq 0 ] && return 0
	fail "Expected: success from '$*'
Actual: exit status $rc
Output: ${output:-<none>}"
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

cleanup()
{
	rm -rf "$BASE" >/dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

require_cmd gcc
require_cmd grep

expect_success_sh "grep -q '^devtmpfs /dev ' /proc/mounts"
expect_success_sh "grep -q '^devpts /dev/pts ' /proc/mounts"
expect_success_sh "test -c /dev/null"
expect_success_sh "test -c /dev/zero"
expect_success_sh "test -c /dev/random"
expect_success_sh "test -c /dev/urandom"
expect_success_sh "test -c /dev/ptmx"
expect_success_sh "test -c /dev/tty1"
expect_success_sh "test -c /dev/console"

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf '%s\n' '#include <sys/stat.h>' > "$SRC"
printf '%s\n' '#include <fcntl.h>' >> "$SRC"
printf '%s\n' '#include <unistd.h>' >> "$SRC"
printf '%s\n' '#include <string.h>' >> "$SRC"
printf '%s\n' '#include <stdio.h>' >> "$SRC"
printf '%s\n' '#include <errno.h>' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' 'static int expect_chr(const char *path)' >> "$SRC"
printf '%s\n' '{' >> "$SRC"
printf '%s\n' '	struct stat st;' >> "$SRC"
printf '%s\n' '	if (stat(path, &st) != 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "stat %s: %s\n", path, strerror(errno));' >> "$SRC"
printf '%s\n' '		return 1;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	if (!S_ISCHR(st.st_mode)) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "%s is not a char device\n", path);' >> "$SRC"
printf '%s\n' '		return 1;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	return 0;' >> "$SRC"
printf '%s\n' '}' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' 'int main(void)' >> "$SRC"
printf '%s\n' '{' >> "$SRC"
printf '%s\n' '	static const char payload[] = "abc";' >> "$SRC"
printf '%s\n' '	unsigned char zeros[8];' >> "$SRC"
printf '%s\n' '	unsigned char rnd[8];' >> "$SRC"
printf '%s\n' '	int fd;' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' '	if (expect_chr("/dev/null") || expect_chr("/dev/zero") ||' >> "$SRC"
printf '%s\n' '	    expect_chr("/dev/random") || expect_chr("/dev/urandom") ||' >> "$SRC"
printf '%s\n' '	    expect_chr("/dev/ptmx") || expect_chr("/dev/tty1") ||' >> "$SRC"
printf '%s\n' '	    expect_chr("/dev/console"))' >> "$SRC"
printf '%s\n' '		return 1;' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' '	fd = open("/dev/null", O_WRONLY);' >> "$SRC"
printf '%s\n' '	if (fd < 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "open /dev/null: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		return 2;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	if (write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1)) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "write /dev/null failed\n");' >> "$SRC"
printf '%s\n' '		close(fd);' >> "$SRC"
printf '%s\n' '		return 3;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	close(fd);' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' '	fd = open("/dev/zero", O_RDONLY);' >> "$SRC"
printf '%s\n' '	if (fd < 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "open /dev/zero: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		return 4;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	memset(zeros, 0xff, sizeof(zeros));' >> "$SRC"
printf '%s\n' '	if (read(fd, zeros, sizeof(zeros)) != (ssize_t)sizeof(zeros)) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "read /dev/zero failed\n");' >> "$SRC"
printf '%s\n' '		close(fd);' >> "$SRC"
printf '%s\n' '		return 5;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	close(fd);' >> "$SRC"
printf '%s\n' '	for (fd = 0; fd < (int)sizeof(zeros); fd++) {' >> "$SRC"
printf '%s\n' '		if (zeros[fd] != 0) {' >> "$SRC"
printf '%s\n' '			fprintf(stderr, "/dev/zero returned non-zero data\n");' >> "$SRC"
printf '%s\n' '			return 6;' >> "$SRC"
printf '%s\n' '		}' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' '	fd = open("/dev/urandom", O_RDONLY);' >> "$SRC"
printf '%s\n' '	if (fd < 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "open /dev/urandom: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		return 7;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	if (read(fd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "read /dev/urandom failed\n");' >> "$SRC"
printf '%s\n' '		close(fd);' >> "$SRC"
printf '%s\n' '		return 8;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	close(fd);' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' '	return 0;' >> "$SRC"
printf '%s\n' '}' >> "$SRC"

expect_success gcc -o "$BIN" "$SRC"
[ -x "$BIN" ] || fail "compiled helper missing"
set +e
output=$("$BIN" 2>&1)
rc=$?
set -e
[ "$rc" -eq 0 ] || fail "Expected: success from '$BIN'
Actual: exit status $rc
Output: ${output:-<none>}"
