#!/bin/sh
set -eu

base=$(mktemp -d /tmp/ldt-context.XXXXXX)
trap 'rm -rf "$base"' EXIT HUP INT TERM
cat > "$base/probe.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sched.h>

struct descriptor {
    unsigned entry, base, limit, flags;
};

static unsigned short ldtr(void)
{
    unsigned short selector;
    __asm__ volatile ("sldt %0" : "=rm" (selector));
    return selector;
}

static void install(unsigned *value)
{
    struct descriptor d = {0, (unsigned)value, sizeof(*value) - 1, 1};
    if (syscall(SYS_modify_ldt, 0x11, &d, sizeof(d)) != 0) {
        perror("modify_ldt");
        exit(1);
    }
}

static void clear_ldt(void)
{
    struct descriptor d = {0, 0, 0, (1 << 3) | (1 << 5)};
    if (syscall(SYS_modify_ldt, 0x11, &d, sizeof(d)) != 0 || ldtr() != 0)
        exit(2);
}

static void verify(unsigned expected)
{
    unsigned value;
    unsigned short old, selector = 7;
    __asm__ volatile (
        "movw %%fs, %0\n\t"
        "movw %2, %%fs\n\t"
        "movl %%fs:0, %1\n\t"
        "movw %0, %%fs"
        : "=&r" (old), "=&r" (value) : "r" (selector) : "memory");
    if (value != expected || ldtr() == 0)
        exit(3);
}

static void wait_ok(pid_t child)
{
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || status != 0)
        exit(4);
}

int main(int argc, char **argv)
{
    unsigned value = 0x12345678;
    pid_t child;
    int i;
    if (argc == 2 && strcmp(argv[1], "empty") == 0)
        return ldtr() != 0;
    if (ldtr() != 0)
        return 5;
    install(&value);
    child = fork();
    if (child == 0) {
        value = 0x87654321;
        install(&value);
        for (i = 0; i < 2000; ++i) {
            getpid();
            sched_yield();
            verify(value);
        }
        clear_ldt();
        for (i = 0; i < 2000; ++i) {
            getpid();
            sched_yield();
            if (ldtr() != 0)
                _exit(6);
        }
        _exit(0);
    }
    for (i = 0; i < 2000; ++i) {
        getpid();
        sched_yield();
        verify(value);
    }
    wait_ok(child);
    child = fork();
    if (child == 0) {
        execl(argv[0], argv[0], "empty", (char *)0);
        _exit(7);
    }
    wait_ok(child);
    verify(value);
    clear_ldt();
    puts("ldt_context: PASS");
    return 0;
}
EOF
gcc -m32 -O2 -Wall -Werror "$base/probe.c" -o "$base/probe"
"$base/probe"
