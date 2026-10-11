#!/bin/sh
# Validate thread-group wait, parent ownership, exit status and orphan reaping.
set -eu
probe_dir=$(mktemp -d /tmp/mos-thread-group-wait.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef CLONE_PARENT_SETTID
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#endif
#ifndef SYS_exit_group
#ifdef __x86_64__
#define SYS_exit_group 231
#else
#define SYS_exit_group 252
#endif
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "thread_group_wait:%d: %s (errno=%d)\n", __LINE__, #x, errno); _exit(2); } } while (0)
/* The clone entry passes TID pointers independently of libc wrapper prototypes. */
extern long clone_start(int (*fn)(void *), void *stack, unsigned flags,
                        void *arg, int *parent_tid, int *child_tid);
#ifdef __x86_64__
__asm__(
    ".text\n.globl clone_start\n.type clone_start,@function\nclone_start:\n"
    "sub $16,%rsi\nmov %rdi,(%rsi)\nmov %rcx,8(%rsi)\n"
    "mov %edx,%edi\nmov %r8,%rdx\nmov %r9,%r10\nxor %r8d,%r8d\n"
    "mov $56,%eax\nsyscall\ntest %rax,%rax\njz 1f\nret\n"
    "1: pop %rax\npop %rdi\ncall *%rax\nmov %eax,%edi\n"
    "mov $60,%eax\nsyscall\nud2\n.size clone_start,.-clone_start\n");
#else
__asm__(
    ".text\n.globl clone_start\n.type clone_start,@function\nclone_start:\n"
    "push %ebp\npush %edi\npush %esi\npush %ebx\n"
    "mov 24(%esp),%ecx\nsub $16,%ecx\nmov 20(%esp),%eax\n"
    "mov %eax,(%ecx)\nmov 32(%esp),%eax\nmov %eax,4(%ecx)\n"
    "mov 28(%esp),%ebx\nmov 36(%esp),%edx\nmov 40(%esp),%edi\n"
    "xor %esi,%esi\nmov $120,%eax\nint $0x80\ntest %eax,%eax\njz 1f\n"
    "pop %ebx\npop %esi\npop %edi\npop %ebp\nret\n"
    "1: pop %eax\npop %ebx\nadd $8,%esp\nsub $12,%esp\npush %ebx\n"
    "call *%eax\nmov %eax,%ebx\nmov $1,%eax\nint $0x80\nud2\n"
    ".size clone_start,.-clone_start\n");
#endif
struct worker { void *stack; volatile int tid, done; int mode, pid, result, status; };
static volatile int child_pid, gate, ready;
static int endpoints[2];
static volatile int *observed_ppid;
static long millis(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}
static void delay(void)
{
    struct timespec ts = { 0, 10000000 };
    syscall(SYS_nanosleep, &ts, NULL);
}
static void await(volatile int *flag)
{
    long end = millis() + 5000;
    while (!*flag && millis() < end) delay();
    CHECK(*flag);
}
static void start(struct worker *w, int (*fn)(void *), int mode)
{
    memset(w, 0, sizeof(*w));
    w->mode = mode;
    w->stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(w->stack != MAP_FAILED);
    CHECK(clone_start(fn, (char *)w->stack + 65536,
                CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                CLONE_THREAD | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID,
                w, (int *)&w->tid, (int *)&w->tid) > 0);
}
static void join(struct worker *w)
{
    long end = millis() + 5000;
    while (w->tid && millis() < end) {
        int tid = w->tid;
        struct timespec ts = { 0, 10000000 };
        syscall(SYS_futex, (int *)&w->tid, 0, tid, &ts, NULL, 0);
    }
    CHECK(!w->tid && w->done);
    CHECK(munmap(w->stack, 65536) == 0);
}
static int finish(struct worker *w)
{
    w->done = 1;
    syscall(SYS_exit, 0);
    return 0;
}
static int proc_state(int pid)
{
    char path[64], text[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int state = '?';
    while (fgets(text, sizeof(text), fp))
        if (!strncmp(text, "State:", 6)) {
            char *p = text + 6;
            while (*p == ' ' || *p == '\t') p++;
            state = *p;
            break;
        }
    fclose(fp);
    return state;
}
static void wait_state(int pid, int state)
{
    long end = millis() + 5000;
    while (proc_state(pid) != state && millis() < end) delay();
    CHECK(proc_state(pid) == state);
}
static int wait_worker(void *arg)
{
    struct worker *w = arg;
    if (w->mode == 1) {
        sigset_t set;
        sigemptyset(&set); sigaddset(&set, SIGCHLD);
        CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
    }
    await(&child_pid);
    int pid = w->mode == 3 ? -child_pid : w->mode == 4 ? -1 : child_pid;
    do {
        w->result = syscall(SYS_wait4, pid, &w->status,
                            w->mode == 2 ? WNOHANG : 0, NULL);
        if (!w->result) delay();
    } while (!w->result);
    return finish(w);
}
static void cross_thread_wait(int mode)
{
    struct worker w;
    start(&w, wait_worker, mode);
    int pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        if (mode == 3) CHECK(setpgid(0, 0) == 0);
        usleep(150000); _exit(19);
    }
    if (mode == 3) CHECK(setpgid(pid, pid) == 0);
    child_pid = pid;
    join(&w);
    CHECK(w.result == pid && WIFEXITED(w.status) && WEXITSTATUS(w.status) == 19);
    CHECK(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    CHECK(!proc_state(pid));
}
static int spawn_worker(void *arg)
{
    struct worker *w = arg;
    int pid = syscall(SYS_fork);
    CHECK(pid >= 0);
    if (!pid) {
        if (!w->mode) usleep(300000);
        *observed_ppid = getppid();
        syscall(SYS_exit_group, 17);
    }
    w->pid = pid; child_pid = pid;
    if (w->mode) await(&gate);
    return finish(w);
}
static void creator_exit(int mode)
{
    struct worker creator, waiter;
    observed_ppid = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(observed_ppid != MAP_FAILED);
    start(&creator, spawn_worker, mode & 1);
    await(&child_pid);
    if (mode & 1) { wait_state(child_pid, 'Z'); gate = 1; }
    join(&creator);
    if (mode & 2) {
        start(&waiter, wait_worker, 0);
        join(&waiter);
        CHECK(waiter.result == child_pid && WEXITSTATUS(waiter.status) == 17);
    } else {
        int status;
        CHECK(waitpid(child_pid, &status, 0) == child_pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 17);
    }
    CHECK(*observed_ppid == getpid());
    CHECK(munmap((void *)observed_ppid, 4096) == 0);
}
static void nohang_selection(int mode)
{
    int first = fork(); CHECK(first >= 0);
    if (!first) { if (mode) CHECK(setpgid(0, 0) == 0); _exit(21); }
    if (mode) CHECK(setpgid(first, first) == 0);
    wait_state(first, 'Z');
    int second = fork(); CHECK(second >= 0);
    if (!second) { if (mode) CHECK(setpgid(0, 0) == 0); usleep(250000); _exit(22); }
    if (mode) CHECK(setpgid(second, second) == 0);
    int status = 0x1234;
    CHECK(waitpid(mode ? -second : second, &status, WNOHANG) == 0);
    CHECK(status == 0x1234 && proc_state(first) == 'Z');
    CHECK(waitpid(mode ? -first : first, &status, 0) == first && WEXITSTATUS(status) == 21);
    CHECK(waitpid(mode ? -second : second, &status, 0) == second && WEXITSTATUS(status) == 22);
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}
static void competing_waiters(int unused)
{
    struct worker a, b;
    int pid = fork(); CHECK(pid >= 0);
    if (!pid) { usleep(200000); _exit(20); }
    child_pid = pid;
    start(&a, wait_worker, 0); start(&b, wait_worker, 0);
    join(&a); join(&b);
    CHECK((a.result == pid && b.result == -1) || (b.result == pid && a.result == -1));
}
static int exit_worker(void *arg)
{
    struct worker *w = arg;
    if (w->mode == 0) syscall(SYS_exit_group, 23);
    if (w->mode == 1) {
        usleep(250000); return finish(w);
    }
    if (w->mode == 2) {
        volatile int *bad = (volatile int *)0;
        *bad = 1;
    }
    ready = 1;
    for (;;) {
        if (w->mode == 3) {
            char byte;
            syscall(SYS_read, endpoints[0], &byte, 1);
        } else syscall(SYS_getpid);
    }
}
static void group_exit(int mode)
{
    int pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        struct worker w;
        start(&w, exit_worker, mode);
        if (mode == 1) { syscall(SYS_exit, 22); for (;;) pause(); }
        for (;;) pause();
    }
    int status;
    if (mode == 1) {
        usleep(100000);
        CHECK(waitpid(pid, &status, WNOHANG) == 0);
    } else if (mode >= 3) {
        usleep(100000);
        CHECK(kill(pid, mode == 3 ? SIGTERM : SIGKILL) == 0);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    if (!mode) CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 23);
    else if (mode == 1) CHECK(WIFEXITED(status));
    else CHECK(WIFSIGNALED(status) && WTERMSIG(status) ==
               (mode == 2 ? SIGSEGV : mode == 3 ? SIGTERM : SIGKILL));
    CHECK(!proc_state(pid));
}
static int orphan_worker(void *arg)
{
    struct worker *w = arg;
    int pid = syscall(SYS_fork); CHECK(pid >= 0);
    if (!pid) {
        if (!w->mode) {
            usleep(400000);
            int parent = getppid();
            CHECK(write(endpoints[1], &parent, sizeof(parent)) == sizeof(parent));
        }
        syscall(SYS_exit_group, 18);
    }
    CHECK(write(endpoints[1], &pid, sizeof(pid)) == sizeof(pid));
    if (w->mode) wait_state(pid, 'Z');
    return finish(w);
}
static void orphan_reaping(int mode)
{
    CHECK(pipe(endpoints) == 0);
    int pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        struct worker w;
        close(endpoints[0]);
        start(&w, orphan_worker, mode);
        join(&w);
        _exit(0);
    }
    close(endpoints[1]);
    int orphan, status;
    CHECK(read(endpoints[0], &orphan, sizeof(orphan)) == sizeof(orphan));
    CHECK(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);
    if (!mode) {
        int parent;
        CHECK(read(endpoints[0], &parent, sizeof(parent)) == sizeof(parent));
        CHECK(parent == 1);
    }
    close(endpoints[0]);
    wait_state(orphan, 0);
}
static void exit_pipe_eof(int mode)
{
    CHECK(pipe(endpoints) == 0);
    int pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        struct worker w;
        close(endpoints[0]);
        start(&w, exit_worker, mode ? 4 : 0);
        if (mode) { usleep(100000); syscall(SYS_exit_group, 24); }
        for (;;) pause();
    }
    close(endpoints[1]);
    char byte; int status;
    CHECK(read(endpoints[0], &byte, 1) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    close(endpoints[0]);
}
static void parent_death_signal(int unused)
{
    CHECK(pipe(endpoints) == 0);
    int pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        int nested = fork(); CHECK(nested >= 0);
        if (!nested) {
            CHECK(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0);
            int self = getpid();
            CHECK(write(endpoints[1], &self, sizeof(self)) == sizeof(self));
            for (;;) pause();
        }
        usleep(250000); _exit(0);
    }
    close(endpoints[1]);
    int nested, status;
    CHECK(read(endpoints[0], &nested, sizeof(nested)) == sizeof(nested));
    close(endpoints[0]);
    CHECK(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);
    wait_state(nested, 0);
}
static void run_case(const char *name, void (*fn)(int), int mode)
{
    int pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        alarm(12);
        child_pid = gate = ready = 0;
        fn(mode); _exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "FAIL: %s (status=%d)\n", name, status);
        exit(2);
    }
    printf("PASS: %s\n", name); fflush(stdout);
}
int main(void)
{
    alarm(90);
    run_case("worker waits for leader child", cross_thread_wait, 0);
    run_case("blocked SIGCHLD does not suppress wait wakeup", cross_thread_wait, 1);
    run_case("worker WNOHANG polling", cross_thread_wait, 2);
    run_case("worker waits for process group", cross_thread_wait, 3);
    run_case("worker waits for any child", cross_thread_wait, 4);
    run_case("live child survives creator thread exit", creator_exit, 0);
    run_case("zombie survives creator thread exit until wait", creator_exit, 1);
    run_case("sibling waits for live child", creator_exit, 2);
    run_case("sibling waits for zombie", creator_exit, 3);
    run_case("WNOHANG preserves unmatched zombie", nohang_selection, 0);
    run_case("group WNOHANG preserves unmatched zombie", nohang_selection, 1);
    run_case("only one competing waiter reaps a child", competing_waiters, 0);
    run_case("nonleader exit_group status", group_exit, 0);
    run_case("leader exit waits for remaining thread", group_exit, 1);
    run_case("worker fault terminates thread group", group_exit, 2);
    CHECK(pipe(endpoints) == 0);
    run_case("SIGTERM terminates blocked thread group", group_exit, 3);
    close(endpoints[0]); close(endpoints[1]);
    run_case("SIGKILL terminates running thread group", group_exit, 4);
    run_case("init adopts live thread-created child", orphan_reaping, 0);
    run_case("init reaps thread-created zombie", orphan_reaping, 1);
    run_case("nonleader exit_group closes shared pipe", exit_pipe_eof, 0);
    run_case("leader exit_group closes shared pipe", exit_pipe_eof, 1);
    run_case("parent death signal terminates adopted child", parent_death_signal, 0);
    puts("PASS: thread-group and parent-child lifecycle");
    return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
