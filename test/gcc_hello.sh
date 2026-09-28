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
#include <unistd.h>

/* RH9 libc predates getauxval; read the same kernel-provided vector. */
static unsigned long *auxv;
static unsigned long aux_value(unsigned long type)
{
	unsigned long *p;
	for (p = auxv; p[0]; p += 2)
		if (p[0] == type) return p[1];
	return 0;
}

#ifdef LEGACY_PIE
/* Old binutils can build ET_DYN via -shared but need an explicit interpreter. */
const char interpreter[] __attribute__((section(".interp"))) = "/lib/ld-linux.so.2";
/* RH9's executable startup expects array bounds absent from its shared linker
 * script.  This fixture has no init/fini arrays; retain the normal CRT hooks. */
extern void _init(void), _fini(void);
void __libc_csu_init(int argc, char **argv, char **envp) { _init(); }
void __libc_csu_fini(void) { _fini(); }
#endif

static int value = 42;
static int *pointer = &value;
static char bss[8192];
static __thread int tls_value = 7;

static void goodbye(void)
{
	puts("pie exit");
}

int main(int argc, char **argv, char **envp)
{
	while (*envp) envp++;
	auxv = (unsigned long *)(envp + 1);
	Elf32_Phdr *ph = (void *)aux_value(AT_PHDR);
	unsigned long entry = aux_value(AT_ENTRY);
	unsigned long count = aux_value(AT_PHNUM);
	unsigned long bias = 0;
	int found = 0;
	if (!ph || !entry || !aux_value(AT_BASE) ||
	    aux_value(AT_PHENT) != sizeof(*ph) || !count)
		return 1;
	for (unsigned long i = 0; i < count; i++)
		if (ph[i].p_type == PT_PHDR)
			bias = (unsigned long)ph - ph[i].p_vaddr;
#ifdef LEGACY_PIE
	/* The old shared linker script omits PT_PHDR but places the ELF and
	 * program headers at the start of the first load segment. */
	Elf32_Ehdr *eh = (void *)((char *)ph - sizeof(Elf32_Ehdr));
	if (eh->e_phoff != sizeof(*eh)) return 6;
	for (unsigned long i = 0; i < count; i++)
		if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0)
			bias = (unsigned long)eh - ph[i].p_vaddr;
#endif
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
# Probe only the driver flags; failures building the actual test remain fatal.
printf 'int main(void) { return 0; }\n' > "$BASE/probe.c"
if gcc -fPIE -pie -o "$BASE/probe" "$BASE/probe.c" >/dev/null 2>&1; then
	expect_success gcc -std=gnu99 -fPIE -pie -o "$BIN" "$SRC"
else
	# A shared executable exercises the same ET_DYN loader on the RH9 toolchain.
	expect_success gcc -std=gnu99 -DLEGACY_PIE -fPIC -shared -Wl,-e,_start \
		-o "$BIN" /usr/lib/crt1.o "$SRC"
fi
expect_success env -i "$BIN"
expect_eq "pie main
pie exit" "$output" "PIE program execution"
