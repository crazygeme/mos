#!/bin/sh
set -e

BASE=/root/tests/posix_vm86_vbe
mkdir -p "$BASE"
cat > "$BASE/vbe.c" <<'EOF'
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/vm86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "vm86_vbe: %s\n", what);
		exit(1);
	}
}

static void prepare(struct vm86_struct *vm, unsigned ax)
{
	memset(vm, 0, sizeof(*vm));
	vm->regs.eax = ax;
	vm->regs.es = 0x1000;
	vm->regs.esp = 0x100;
}

int main(void)
{
	struct vm86_struct vm;
	unsigned char *buffer;
	int i;

	check(sizeof(void *) == 4 && sizeof(vm) == 160, "i386 wire layout");
	buffer = mmap((void *)0x10000, 4096, PROT_READ | PROT_WRITE,
	              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	check(buffer == (void *)0x10000, "low VBE buffer");
	memset(buffer, 0xa5, 4096);
	prepare(&vm, 0x4f00);
	check(syscall(__NR_vm86old, &vm) == VM86_UNKNOWN, "vm86old controller info");
	check((vm.regs.eax & 0xffff) == 0x004f && vm.regs.cs == 0 &&
	      vm.regs.eip == 0x600 && vm.regs.esp == 0x106, "return registers");
	check(memcmp(buffer, "VESA", 4) == 0 &&
	      *(unsigned short *)(buffer + 4) == 0x0300, "VBE controller data");
	for (i = 512; i < 4096; i++)
		check(buffer[i] == 0xa5, "controller buffer bounds");
	memset(buffer, 0xa5, 4096);
	prepare(&vm, 0x4f01);
	vm.regs.ecx = 0x114;
	check(syscall(__NR_vm86, VM86_ENTER, &vm) == VM86_UNKNOWN,
	      "vm86 mode info");
	check((vm.regs.eax & 0xffff) == 0x004f &&
	      *(unsigned short *)(buffer + 18) == 800 &&
	      *(unsigned short *)(buffer + 20) == 600 && buffer[25] == 32,
	      "mode geometry");
	for (i = 256; i < 4096; i++)
		check(buffer[i] == 0xa5, "mode buffer bounds");
	prepare(&vm, 0x4f04);
	vm.regs.ebx = 0xfeed;
	check(syscall(__NR_vm86, VM86_ENTER_NO_BYPASS, &vm) == VM86_UNKNOWN &&
	      (vm.regs.eax & 0xffff) == 0x004f && (vm.regs.ebx & 0xffff) == 1,
	      "state size query");
	check(munmap(buffer, 4096) == 0, "munmap");
	puts("vm86_vbe: PASS");
	return 0;
}
EOF
gcc -Wall -Werror -o "$BASE/vbe" "$BASE/vbe.c"
"$BASE/vbe"
