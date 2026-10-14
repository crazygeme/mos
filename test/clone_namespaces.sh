#!/bin/sh
# Validate namespace identity, process visibility, and filesystem isolation.
set -eu
probe_dir=$(mktemp -d /tmp/mos-namespaces.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC 0x08000000
#endif
#ifndef CLONE_NEWPID
#define CLONE_NEWPID 0x20000000
#endif
#ifndef SYS_unshare
#if defined(__i386__)
#define SYS_unshare 310
#else
#define SYS_unshare 272
#endif
#endif
#define unshare(flags) syscall(SYS_unshare, (flags))
#ifndef SYS_setns
#if defined(__i386__)
#define SYS_setns 346
#else
#define SYS_setns 308
#endif
#endif
#define setns(fd, flags) syscall(SYS_setns, (fd), (flags))
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s: %s\n", __LINE__, #x, strerror(errno)); exit(1); } } while (0)
static void reap(pid_t pid) { int s; CHECK(waitpid(pid, &s, 0) == pid); CHECK(WIFEXITED(s) && !WEXITSTATUS(s)); }
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char old[65]; CHECK(gethostname(old, sizeof(old)) == 0);
    int ns = open("/proc/self/ns/uts", O_RDONLY); CHECK(ns >= 0);
    pid_t child = syscall(SYS_clone, CLONE_NEWUTS | SIGCHLD, 0, 0, 0, 0);
    CHECK(child >= 0);
    if (!child) {
        CHECK(sethostname("namespace-test", 14) == 0);
        char name[65]; CHECK(gethostname(name, sizeof(name)) == 0);
        CHECK(!strcmp(name, "namespace-test"));
        CHECK(setns(ns, CLONE_NEWUTS) == 0);
        CHECK(gethostname(name, sizeof(name)) == 0 && !strcmp(name, old));
        _exit(0);
    }
    reap(child); close(ns);
    char name[65]; CHECK(gethostname(name, sizeof(name)) == 0 && !strcmp(name, old));
    child = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0); CHECK(child >= 0);
    if (!child) {
        CHECK(getpid() == 1 && getppid() == 0);
        CHECK(access("/proc/1/status", R_OK) == 0);
        pid_t inner = fork(); CHECK(inner >= 0);
        if (!inner) { CHECK(getpid() == 2 && getppid() == 1); _exit(0); }
        CHECK(inner == 2); reap(inner); _exit(0);
    }
    reap(child);
    int shm = shmget(0x13572468, 4096, IPC_CREAT | IPC_EXCL | 0600); CHECK(shm >= 0);
    child = syscall(SYS_clone, CLONE_NEWIPC | SIGCHLD, 0, 0, 0, 0); CHECK(child >= 0);
    if (!child) {
        errno = 0; CHECK(shmget(0x13572468, 4096, 0) == -1 && errno == ENOENT);
        int own = shmget(0x13572468, 4096, IPC_CREAT | IPC_EXCL | 0600); CHECK(own >= 0 && own != shm);
        CHECK(shmctl(own, IPC_RMID, 0) == 0); _exit(0);
    }
    reap(child); CHECK(shmctl(shm, IPC_RMID, 0) == 0);
    child = syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0); CHECK(child >= 0);
    if (!child) {
        CHECK(mount("tmpfs", argv[1], "tmpfs", 0, 0) == 0);
        char path[512]; snprintf(path, sizeof(path), "%s/isolated", argv[1]);
        int fd = open(path, O_CREAT | O_WRONLY, 0600); CHECK(fd >= 0); close(fd);
        CHECK(access(path, F_OK) == 0); CHECK(umount(argv[1]) == 0); _exit(0);
    }
    reap(child);
    char path[512]; snprintf(path, sizeof(path), "%s/isolated", argv[1]);
    CHECK(access(path, F_OK) == -1 && errno == ENOENT);
    puts("namespace isolation: PASS");
    return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe" "$probe_dir"
