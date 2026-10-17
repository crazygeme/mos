#!/bin/sh
# Validate waitid event selection, snapshots, and namespace identifiers.
set -eu
probe_dir=$(mktemp -d /tmp/mos-waitid.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'C'
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void exited(pid_t pid, int status, int options)
{
    siginfo_t info;
    memset(&info, 0x55, sizeof(info));
    assert(waitid(P_PID, pid, &info, WEXITED | options) == 0);
    assert(info.si_signo == SIGCHLD && info.si_pid == pid);
    assert(info.si_uid == getuid());
    assert(info.si_code == CLD_EXITED && info.si_status == status);
}
int main(void)
{
    alarm(30);
    siginfo_t info;
    errno = 0;
    assert(waitid(P_ALL, 0, &info, 0) == -1 && errno == EINVAL);
    int gate[2]; assert(pipe(gate) == 0);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { char byte; close(gate[1]); assert(read(gate[0], &byte, 1) == 1); _exit(37); }
    close(gate[0]);
    memset(&info, 0x55, sizeof(info));
    assert(waitid(P_PID, pid, &info, WEXITED | WNOHANG) == 0);
    assert(info.si_pid == 0 && info.si_signo == 0 && info.si_code == 0);
    assert(write(gate[1], "x", 1) == 1); close(gate[1]);
    exited(pid, 37, WNOWAIT); exited(pid, 37, WNOWAIT);
    struct rusage usage; memset(&usage, 0x55, sizeof(usage));
    assert(syscall(SYS_waitid, P_PID, pid, &info, WEXITED, &usage) == 0);
    assert(info.si_pid == pid && info.si_status == 37 && info.si_code == CLD_EXITED);
    assert(usage.ru_utime.tv_sec >= 0 && usage.ru_utime.tv_sec < 30);
    errno = 0; assert(waitid(P_PID, pid, &info, WEXITED) == -1 && errno == ECHILD);
    pid = fork(); assert(pid >= 0);
    if (!pid) { raise(SIGSTOP); _exit(11); }
    assert(waitid(P_PID, pid, &info, WSTOPPED | WNOWAIT) == 0);
    assert(info.si_pid == pid && info.si_code == CLD_STOPPED && info.si_status == SIGSTOP);
    assert(waitid(P_PID, pid, &info, WSTOPPED) == 0);
    assert(info.si_pid == pid && info.si_code == CLD_STOPPED);
    assert(kill(pid, SIGCONT) == 0); exited(pid, 11, 0);
    pid = fork(); assert(pid >= 0);
    if (!pid) { for (;;) pause(); }
    assert(kill(pid, SIGTERM) == 0);
    assert(waitid(P_ALL, 0, &info, WEXITED) == 0);
    assert(info.si_pid == pid && info.si_code == CLD_KILLED && info.si_status == SIGTERM);
    pid = fork(); assert(pid >= 0);
    if (!pid) _exit(23);
    assert(waitid(P_PGID, 0, &info, WEXITED) == 0);
    assert(info.si_pid == pid && info.si_code == CLD_EXITED && info.si_status == 23);
    if (!getuid()) {
        pid = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0); assert(pid >= 0);
        if (!pid) {
            assert(getpid() == 1);
            pid_t child = fork(); assert(child >= 0);
            if (!child) _exit(19);
            assert(child == 2); exited(child, 19, WNOWAIT); exited(child, 19, 0); _exit(0);
        }
        exited(pid, 0, 0);
    }
    puts("waitid exit, stop, nohang, nowait, usage, and PID namespace: PASS");
    return 0;
}
C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
