#!/bin/sh
set -e

BASE=/root/tests/posix_sysv_shm
mkdir -p "$BASE"
cat > "$BASE/shm.c" <<'EOF'
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "sysv_shm: %s\n", what);
		exit(1);
	}
}

int main(void)
{
	struct {
		unsigned int address;
		unsigned int guard;
	} result;
	int id;
	char *first, *second;

	check(sizeof(void *) == 4, "requires i386 userspace");
	check(((unsigned long)&result.address & 0x80000000UL) != 0,
	      "result pointer must have bit 31 set");
	id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
	check(id >= 0, "shmget");
	result.address = 0;
	result.guard = 0x12345678;
	/* i386 IPC SHMAT returns its address through the third argument. */
	check(syscall(__NR_ipc, 21, id, 0, &result.address, NULL, 0) == 0,
	      "raw shmat with high stack result pointer");
	check(result.address != 0 && result.guard == 0x12345678,
	      "four-byte result store");
	first = (char *)(unsigned long)result.address;
	check(first[0] == 0, "initial contents");
	strcpy(first, "shared-memory");
	second = shmat(id, NULL, 0);
	check(second != (void *)-1, "glibc shmat");
	check(strcmp(second, "shared-memory") == 0, "shared backing");
	check(shmctl(id, IPC_RMID, NULL) == 0, "IPC_RMID");
	strcpy(second, "still-attached");
	check(strcmp(first, "still-attached") == 0, "removed segment backing");
	check(shmdt(second) == 0 && shmdt(first) == 0, "shmdt");
	puts("sysv_shm: PASS");
	return 0;
}
EOF
gcc -Wall -Werror -o "$BASE/shm" "$BASE/shm.c"
"$BASE/shm"
