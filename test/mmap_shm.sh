#!/bin/sh

set -e
CASE_NAME=mmap_shm
BASE=/root/tests/$CASE_NAME
SRC="$BASE/mmap_shm.c"
BIN="$BASE/mmap_shm"

fail()
{
	echo "mmap_shm: $1" >&2
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

expect_success_sh "grep -q '^tmpfs /dev/shm ' /proc/mounts"

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf '%s\n' '#include <sys/mman.h>' > "$SRC"
printf '%s\n' '#include <sys/stat.h>' >> "$SRC"
printf '%s\n' '#include <sys/wait.h>' >> "$SRC"
printf '%s\n' '#include <fcntl.h>' >> "$SRC"
printf '%s\n' '#include <unistd.h>' >> "$SRC"
printf '%s\n' '#include <stdio.h>' >> "$SRC"
printf '%s\n' '#include <string.h>' >> "$SRC"
printf '%s\n' '#include <errno.h>' >> "$SRC"
printf '%s\n' '#include <stdlib.h>' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' '#define FILE_PATH "/dev/shm/mmap_shm.bin"' >> "$SRC"
printf '%s\n' '#define MAP_LEN 4096' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' 'static int fail(const char *msg)' >> "$SRC"
printf '%s\n' '{' >> "$SRC"
printf '%s\n' '	fprintf(stderr, "%s\n", msg);' >> "$SRC"
printf '%s\n' '	return 1;' >> "$SRC"
printf '%s\n' '}' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' 'int main(void)' >> "$SRC"
printf '%s\n' '{' >> "$SRC"
printf '%s\n' '	int fd, rc;' >> "$SRC"
printf '%s\n' '	char buf[64];' >> "$SRC"
printf '%s\n' '	char *a, *b;' >> "$SRC"
printf '%s\n' '	pid_t pid;' >> "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' '	unlink(FILE_PATH);' >> "$SRC"
printf '%s\n' '	fd = open(FILE_PATH, O_CREAT | O_RDWR | O_TRUNC, 0600);' >> "$SRC"
printf '%s\n' '	if (fd < 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "open: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		return 2;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	if (ftruncate(fd, MAP_LEN) != 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "ftruncate: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		close(fd);' >> "$SRC"
printf '%s\n' '		return 3;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	a = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);' >> "$SRC"
printf '%s\n' '	if (a == MAP_FAILED) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "mmap a: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		close(fd);' >> "$SRC"
printf '%s\n' '		return 4;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	b = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);' >> "$SRC"
printf '%s\n' '	if (b == MAP_FAILED) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "mmap b: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		munmap(a, MAP_LEN);' >> "$SRC"
printf '%s\n' '		close(fd);' >> "$SRC"
printf '%s\n' '		return 5;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	memset(a, 0, MAP_LEN);' >> "$SRC"
printf '%s\n' '	strcpy(a, "same-process-update");' >> "$SRC"
printf '%s\n' '	if (strcmp(b, "same-process-update") != 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "second mapping did not observe MAP_SHARED write\n");' >> "$SRC"
printf '%s\n' '		munmap(b, MAP_LEN); munmap(a, MAP_LEN); close(fd);' >> "$SRC"
printf '%s\n' '		return 6;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	pid = fork();' >> "$SRC"
printf '%s\n' '	if (pid < 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "fork: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		munmap(b, MAP_LEN); munmap(a, MAP_LEN); close(fd);' >> "$SRC"
printf '%s\n' '		return 7;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	if (pid == 0) {' >> "$SRC"
printf '%s\n' '		strcpy(a, "fork-shared-update");' >> "$SRC"
printf '%s\n' '		_exit(0);' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	if (waitpid(pid, &rc, 0) < 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "waitpid: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		munmap(b, MAP_LEN); munmap(a, MAP_LEN); close(fd);' >> "$SRC"
printf '%s\n' '		return 8;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0)' >> "$SRC"
printf '%s\n' '		return fail("child process failed");' >> "$SRC"
printf '%s\n' '	if (strcmp(b, "fork-shared-update") != 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "parent mapping did not observe child MAP_SHARED write\n");' >> "$SRC"
printf '%s\n' '		munmap(b, MAP_LEN); munmap(a, MAP_LEN); close(fd);' >> "$SRC"
printf '%s\n' '		return 9;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	munmap(b, MAP_LEN);' >> "$SRC"
printf '%s\n' '	munmap(a, MAP_LEN);' >> "$SRC"
printf '%s\n' '	close(fd);' >> "$SRC"
printf '%s\n' '	fd = open(FILE_PATH, O_RDONLY);' >> "$SRC"
printf '%s\n' '	if (fd < 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "reopen: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		return 10;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	memset(buf, 0, sizeof(buf));' >> "$SRC"
printf '%s\n' '	if (read(fd, buf, sizeof(buf) - 1) <= 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "readback failed\n");' >> "$SRC"
printf '%s\n' '		close(fd);' >> "$SRC"
printf '%s\n' '		return 11;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	close(fd);' >> "$SRC"
printf '%s\n' '	if (strcmp(buf, "fork-shared-update") != 0)' >> "$SRC"
printf '%s\n' '		return fail("file contents did not persist MAP_SHARED write");' >> "$SRC"
printf '%s\n' '	fd = open(FILE_PATH, O_RDONLY);' >> "$SRC"
printf '%s\n' '	if (fd < 0)' >> "$SRC"
printf '%s\n' '		return fail("second reopen failed");' >> "$SRC"
printf '%s\n' '	a = mmap(NULL, MAP_LEN, PROT_READ, MAP_SHARED, fd, 0);' >> "$SRC"
printf '%s\n' '	if (a == MAP_FAILED) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "readonly mmap: %s\n", strerror(errno));' >> "$SRC"
printf '%s\n' '		close(fd);' >> "$SRC"
printf '%s\n' '		return 12;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	if (strcmp(a, "fork-shared-update") != 0) {' >> "$SRC"
printf '%s\n' '		fprintf(stderr, "readonly remap did not see shared file contents\n");' >> "$SRC"
printf '%s\n' '		munmap(a, MAP_LEN); close(fd);' >> "$SRC"
printf '%s\n' '		return 13;' >> "$SRC"
printf '%s\n' '	}' >> "$SRC"
printf '%s\n' '	munmap(a, MAP_LEN);' >> "$SRC"
printf '%s\n' '	close(fd);' >> "$SRC"
printf '%s\n' '	unlink(FILE_PATH);' >> "$SRC"
printf '%s\n' '	return 0;' >> "$SRC"
printf '%s\n' '}' >> "$SRC"

expect_success gcc -o "$BIN" "$SRC"
[ -x "$BIN" ] || fail "compiled helper missing"
expect_success "$BIN"
