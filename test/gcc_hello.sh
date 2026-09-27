#!/bin/sh

set -e
CASE_NAME=gcc_hello
BASE=/root/tests/$CASE_NAME
SRC="$BASE/hello.c"
BIN="$BASE/hello"
OUT="$BASE/out.txt"

fail()
{
	echo "gcc_hello: $1" >&2
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

expect_eq()
{
	[ "$1" = "$2" ] && return 0
	fail "Expected: $3 = '$1'
Actual: '$2'"
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
printf '%s\n' '#include <stdio.h>' > "$SRC"
printf '%s\n' '' >> "$SRC"
printf '%s\n' 'int main(void)' >> "$SRC"
printf '%s\n' '{' >> "$SRC"
printf '%s\n' '	puts("hello, world");' >> "$SRC"
printf '%s\n' '	return 0;' >> "$SRC"
printf '%s\n' '}' >> "$SRC"

expect_success gcc -o "$BIN" "$SRC"
[ -x "$BIN" ] || fail "compiled binary missing"

set +e
"$BIN" > "$OUT" 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || fail "Expected: success from '$BIN'
Actual: exit status $rc
Output: $(cat "$OUT" 2>/dev/null || printf '<none>')"
output=$(cat "$OUT" 2>/dev/null) || fail "read output failed"
expect_eq "hello, world" "$output" "compiled program output"

# Static libc uses the ELF entry %edx as an optional exit callback.
# A stale stack pointer there crashes after main and atexit handlers return.
cat > "$SRC" <<'EOF'
#include <stdio.h>
#include <stdlib.h>

static void goodbye(void)
{
	puts("atexit called");
}

int main(void)
{
	if (atexit(goodbye) != 0)
		return 1;
	puts("static main");
	return 0;
}
EOF
expect_success gcc -static -o "$BIN" "$SRC"
expect_success "$BIN"
expect_eq "static main
atexit called" "$output" "static program exit output"

# PIE relocation, BSS, TLS, auxiliary vectors, and exit callbacks.
cat > "$SRC" <<'EOF'
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <unistd.h>

static int value = 42;
static int *pointer = &value;
static char bss[8192];
static __thread int tls_value = 7;

static void goodbye(void)
{
	puts("pie exit");
}

int main(void)
{
	Elf32_Phdr *ph = (void *)getauxval(AT_PHDR);
	unsigned long entry = getauxval(AT_ENTRY);
	unsigned long count = getauxval(AT_PHNUM);
	unsigned long bias = 0;
	int found = 0;
	if (!ph || !entry || !getauxval(AT_BASE) ||
	    getauxval(AT_PHENT) != sizeof(*ph) || !count)
		return 1;
	for (unsigned long i = 0; i < count; i++)
		if (ph[i].p_type == PT_PHDR)
			bias = (unsigned long)ph - ph[i].p_vaddr;
	for (unsigned long i = 0; i < count; i++)
		if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X) &&
		    entry >= bias + ph[i].p_vaddr &&
		    entry < bias + ph[i].p_vaddr + ph[i].p_memsz)
			found = 1;
	if (!bias || !found || *pointer != 42 || tls_value != 7)
		return 2;
	for (unsigned i = 0; i < sizeof(bss); i++)
		if (bss[i]) return 3;
	if (sbrk(0) <= (void *)(bss + sizeof(bss)))
		return 4;
	if (atexit(goodbye)) return 5;
	puts("pie main");
	return 0;
}
EOF
expect_success gcc -fPIE -pie -o "$BIN" "$SRC"
expect_success env -i "$BIN"
expect_eq "pie main
pie exit" "$output" "PIE program execution"
