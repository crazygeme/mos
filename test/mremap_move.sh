#!/bin/sh
# Validate relocated mapping permissions, backing, and page references.
set -eu
probe_dir=$(mktemp -d /tmp/mos-mremap-move.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif
static void *move_mapping(void *address, size_t old_size, size_t new_size,
                          int flags, void *target)
{
    return (void *)syscall(SYS_mremap, address, old_size, new_size, flags, target);
}
static size_t p;
static char *reserve(size_t n, int prot)
{
    char *s = mmap(NULL, n, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(s != MAP_FAILED);
    return s;
}

int main(void)
{
    alarm(30);
    p = sysconf(_SC_PAGESIZE);
    char *s = reserve(3 * p, PROT_READ | PROT_WRITE);
    s[0] = 11; s[p] = 22;
    assert(mprotect(s, 2 * p, PROT_READ) == 0);
    assert(mprotect(s + 2 * p, p, PROT_NONE) == 0);
    char *t = move_mapping(s, 2 * p, 4 * p, MREMAP_MAYMOVE, NULL);
    assert(t != MAP_FAILED && t != s);
    assert(t[0] == 11 && t[p] == 22 && t[3 * p] == 0);
    assert(mprotect(t, 4 * p, PROT_READ | PROT_WRITE) == 0);
    t[0] = 33;
    assert(munmap(t, 4 * p) == 0);
    assert(munmap(s + 2 * p, p) == 0);

    s = reserve(p, PROT_READ | PROT_WRITE); s[0] = 44;
    assert(mprotect(s, p, PROT_NONE) == 0);
    t = reserve(p, PROT_NONE);
    assert(move_mapping(s, p, p, MREMAP_MAYMOVE | MREMAP_FIXED, t) == t);
    assert(mprotect(t, p, PROT_READ) == 0 && t[0] == 44);
    assert(munmap(t, p) == 0);

    char path[] = "/tmp/mos-mremap-file.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && unlink(path) == 0 && ftruncate(fd, 2 * p) == 0);
    char value = 55;
    assert(pwrite(fd, &value, 1, p) == 1);
    s = reserve(2 * p, PROT_NONE);
    assert(mmap(s, p, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == s);
    s[0] = 66;
    t = move_mapping(s, p, 2 * p, MREMAP_MAYMOVE, NULL);
    assert(t != MAP_FAILED && t != s && t[0] == 66 && t[p] == 55);
    t[0] = 77;
    assert(munmap(t, 2 * p) == 0);
    value = 0;
    assert(pread(fd, &value, 1, 0) == 1 && value == 77);
    char *anonymous = mmap(NULL, p, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, fd, p);
    assert(anonymous != MAP_FAILED && anonymous[0] == 0);
    anonymous[0] = 123;
    assert(pread(fd, &value, 1, p) == 1 && value == 55);
    assert(munmap(anonymous, p) == 0);
    anonymous = mmap(NULL, p, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, 0x7fffffff, p);
    assert(anonymous != MAP_FAILED && anonymous[0] == 0);
    assert(munmap(anonymous, p) == 0);
    assert(close(fd) == 0);
    assert(munmap(s + p, p) == 0);

    size_t sparse = 32 * 1024 * 1024;
    s = reserve(sparse, PROT_NONE); t = reserve(sparse, PROT_NONE);
    assert(move_mapping(s, sparse, sparse, MREMAP_MAYMOVE | MREMAP_FIXED, t) == t);
    assert(mprotect(t + sparse - p, p, PROT_READ | PROT_WRITE) == 0);
    assert(t[sparse - 1] == 0); t[sparse - 1] = 88;
    assert(munmap(t, sparse) == 0);

    s = reserve(p, PROT_READ | PROT_WRITE); s[0] = 99;
    int ready[2], done[2];
    assert(pipe(ready) == 0 && pipe(done) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(write(ready[1], "x", 1) == 1);
        char c;
        assert(read(done[0], &c, 1) == 1 && s[0] == 99);
        _exit(0);
    }
    char c;
    assert(read(ready[0], &c, 1) == 1);
    t = reserve(p, PROT_NONE);
    assert(move_mapping(s, p, p, MREMAP_MAYMOVE | MREMAP_FIXED, t) == t);
    t[0] = 100;
    assert(write(done[1], "x", 1) == 1);
    int status;
    assert(waitpid(child, &status, 0) == child && status == 0);
    assert(t[0] == 100 && munmap(t, p) == 0);
    for (int i = 0; i < 2; i++) { close(ready[i]); close(done[i]); }
    puts("mremap relocation: PASS");
    return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
