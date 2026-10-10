#!/bin/sh
# Validate file-backed input faults during concurrent ext4 writes and reads.
set -eu
probe_dir=$(mktemp -d /tmp/mos-ext4-fault.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static size_t length = 4 * 1024 * 1024;
static char *input;
static int output;
static volatile int done;
static int writer(void *unused)
{
    (void)unused;
    for (int i = 0; i < 8; i++) {
        assert(madvise(input, length, MADV_DONTNEED) == 0);
        assert(pwrite(output, input, length, 0) == (ssize_t)length);
    }
    int value = 1;
    __asm__ __volatile__("xchgl %0, %1"
                         : "+r"(value), "+m"(done) : : "memory");
    return 0;
}
int main(void)
{
    alarm(60);
    char path[] = "/tmp/mos-ext4-source.XXXXXX";
    int source = mkstemp(path);
    assert(source >= 0 && unlink(path) == 0);
    char *data = malloc(length);
    assert(data);
    memset(data, 37, length);
    assert(write(source, data, length) == (ssize_t)length);
    free(data);
    input = mmap(NULL, length, PROT_READ, MAP_PRIVATE, source, 0);
    char *alias = mmap(NULL, length, PROT_READ, MAP_PRIVATE, source, 0);
    assert(input != MAP_FAILED && alias != MAP_FAILED);
    char target[] = "/tmp/mos-ext4-target.XXXXXX";
    output = mkstemp(target);
    assert(output >= 0 && unlink(target) == 0);
    size_t n = 64 * 1024;
    char *stack = mmap(NULL, n, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(stack != MAP_FAILED);
    pid_t child = clone(writer, stack + n,
                       CLONE_VM | CLONE_FILES | CLONE_FS | CLONE_SIGHAND | SIGCHLD,
                       NULL);
    assert(child >= 0);
    while (!done) {
        assert(madvise(alias, length, MADV_DONTNEED) == 0);
        for (size_t i = 0; i < length; i += 4096)
            assert(*(volatile char *)(alias + i) == 37);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && status == 0);
    char value;
    assert(pread(output, &value, 1, length - 1) == 1 && value == 37);
    assert(mprotect(alias, length, PROT_NONE) == 0);
    errno = 0;
    assert(write(output, alias, 4096) == -1 && errno == EFAULT);
    assert(close(source) == 0 && close(output) == 0);
    munmap(input, length); munmap(alias, length); munmap(stack, n);
    puts("ext4 write faults: PASS");
    return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
