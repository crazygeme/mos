#!/bin/sh
# Validate dumpability, process inspection, and CLONE_FS across exec.
set -eu
probe_dir=$(mktemp -d /tmp/mos-suid-sandbox.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'C'
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#define CALL_OPENAT 257
#else
#define CALL_OPENAT 295
#endif

static void wait_ok(pid_t pid)
{
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "exec-fs")) {
        assert(chdir(argv[2]) == 0);
        return 0;
    }
    assert(argc == 2);
    int *wait_status = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(wait_status != MAP_FAILED);
    *wait_status = -1;
    pid_t wait_child = fork();
    assert(wait_child >= 0);
    if (!wait_child)
        _exit(37);
    assert(waitpid(wait_child, wait_status, 0) == wait_child);
    assert(WIFEXITED(*wait_status) && WEXITSTATUS(*wait_status) == 37);
    assert(munmap(wait_status, 4096) == 0);
    int proc = open("/proc", O_RDONLY | O_DIRECTORY);
    assert(proc >= 0);
    int duplicate = syscall(CALL_OPENAT, proc, "././.", O_RDONLY | O_DIRECTORY, 0);
    assert(duplicate >= 0);
    struct stat original, copied;
    assert(fstat(proc, &original) == 0 && fstat(duplicate, &copied) == 0);
    assert(original.st_dev == copied.st_dev && original.st_ino == copied.st_ino);
    assert(close(duplicate) == 0 && close(proc) == 0);
#if __SIZEOF_LONG__ == 8
    pid_t limit_child = fork();
    assert(limit_child >= 0);
    if (!limit_child) {
        struct rlimit limit = { 8ULL << 30, 8ULL << 30 }, observed;
        assert(setrlimit(RLIMIT_DATA, &limit) == 0);
        assert(getrlimit(RLIMIT_DATA, &observed) == 0);
        assert(observed.rlim_cur == limit.rlim_cur);
        assert(observed.rlim_max == limit.rlim_max);
        limit.rlim_cur++;
        errno = 0;
        assert(setrlimit(RLIMIT_DATA, &limit) == -1 && errno == EINVAL);
        _exit(0);
    }
    wait_ok(limit_child);
#endif
    assert(prctl(PR_GET_DUMPABLE) == 1);
    assert(prctl(PR_SET_DUMPABLE, 0) == 0);
    assert(prctl(PR_GET_DUMPABLE) == 0);
    errno = 0;
    assert(prctl(PR_SET_DUMPABLE, 2) == -1 && errno == EINVAL);
    assert(prctl(PR_GET_DUMPABLE) == 0);
    pid_t parent = getpid();
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        char path[64];
        assert(prctl(PR_GET_DUMPABLE) == 0);
        assert(prctl(PR_SET_DUMPABLE, 1) == 0);
        if (getuid() != 0) {
            snprintf(path, sizeof(path), "/proc/%d/environ", parent);
            assert(open(path, O_RDONLY) == -1);
            snprintf(path, sizeof(path), "/proc/%d/fd/0", parent);
            assert(open(path, O_RDONLY) == -1);
        }
        _exit(0);
    }
    wait_ok(child);
    assert(prctl(PR_GET_DUMPABLE) == 0);
    assert(prctl(PR_SET_DUMPABLE, 1) == 0);
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    child = syscall(SYS_clone, CLONE_FS | SIGCHLD, 0, 0, 0, 0);
    assert(child >= 0);
    if (!child) {
        execl(argv[0], argv[0], "exec-fs", argv[1], (char *)0);
        _exit(1);
    }
    wait_ok(child);
    char shared[4096];
    assert(getcwd(shared, sizeof(shared)));
    assert(!strcmp(shared, argv[1]));
    assert(chdir(cwd) == 0);
    puts("SUID sandbox prerequisites: PASS");
    return 0;
}
C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe" "$probe_dir"
