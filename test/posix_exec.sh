#!/bin/sh

set -e
CASE_NAME=posix_exec
BASE=/root/tests/$CASE_NAME
FILE="$BASE/run.sh"
SRC="$BASE/exec_script.c"
BIN="$BASE/exec_script"

fail()
{
	echo "posix_exec: $1" >&2
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

mkdir -p "$BASE" >/dev/null 2>&1 || fail "mkdir failed"
printf '#!/bin/sh\nexit 7\n' > "$FILE"

chmod 0644 "$FILE" >/dev/null 2>&1 || fail "chmod 0644 failed"
expect_failure "$FILE"

chmod 0755 "$FILE" >/dev/null 2>&1 || fail "chmod 0755 failed"
set +e
"$FILE" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 7 ] || fail "Expected: script exit status 7
Actual: ${rc}"

set +e
sh "$FILE" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 7 ] || fail "Expected: 'sh $FILE' exit status 7
Actual: ${rc}"

printf '#!/bin/sh\n[ "$0" = "%s" ] || exit 31\nexit 23\n' "$FILE" > "$FILE"
cat > "$SRC" <<EOF
#include <unistd.h>

extern char **environ;

int main(void)
{
	char *argv[] = { "not-the-script-path", "arg1", 0 };
	execve("$FILE", argv, environ);
	return 99;
}
EOF
expect_success gcc "$SRC" -o "$BIN"
set +e
"$BIN" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 23 ] || fail "Expected: shebang passes script path as argv[1]
Actual: ${rc}"

# execve rejects invalid ELF and missing interpreters without replacing the caller.
printf 'int main(void) { return 0; }\n' > "$SRC"
expect_success gcc -fPIE -pie -Wl,--dynamic-linker=/nonexistent/mos-test-ld -o "$BASE/missing-interp" "$SRC"
cat > "$SRC" <<'EOF'
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int rejected(const char *path, int error)
{
	char *args[] = { (char *)path, NULL };
	char *env[] = { NULL };
	int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (fd < 0) return 1;
	int result = execve(path, args, env);
	int saved_errno = errno;
	int alive = fcntl(fd, F_GETFD);
	close(fd);
	return result != -1 || saved_errno != error || alive < 0;
}

int main(int argc, char **argv)
{
	Elf32_Ehdr h = {0};
	if (argc != 3) return 1;
	memcpy(h.e_ident, ELFMAG, SELFMAG);
	h.e_ident[EI_CLASS] = ELFCLASS32;
	h.e_ident[EI_DATA] = ELFDATA2LSB;
	h.e_ident[EI_VERSION] = EV_CURRENT;
	h.e_type = ET_EXEC;
	h.e_machine = EM_386;
	h.e_version = EV_CURRENT;
	h.e_ehsize = sizeof(h);
	h.e_phoff = sizeof(h);
	h.e_phentsize = sizeof(Elf32_Phdr);
	h.e_phnum = 1;
	int fd = open(argv[1], O_CREAT | O_TRUNC | O_WRONLY, 0700);
	if (fd < 0 || write(fd, &h, sizeof(h)) != sizeof(h)) return 2;
	close(fd);
	if (rejected(argv[1], ENOEXEC)) return 3;
	if (rejected(argv[2], ENOENT)) return 4;
	puts("exec errors preserved caller");
	return 0;
}
EOF
expect_success gcc -fno-pie -no-pie -o "$BIN" "$SRC"
expect_success "$BIN" "$BASE/invalid-elf" "$BASE/missing-interp"
