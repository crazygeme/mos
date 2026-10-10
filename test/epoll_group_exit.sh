#!/bin/sh
# Validate epoll ownership across concurrent thread-group termination.
set -eu
probe_dir=$(mktemp -d /tmp/mos-epoll-exit.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef SYS_epoll_create
#if defined(__i386__)
#define SYS_epoll_create 254
#define SYS_epoll_ctl 255
#define SYS_epoll_wait 256
#elif defined(__x86_64__)
#define SYS_epoll_create 213
#define SYS_epoll_ctl 233
#define SYS_epoll_wait 232
#else
#error Unsupported syscall ABI
#endif
#endif
static int epfd;
static int create_epoll(void)
{
    return syscall(SYS_epoll_create, 1);
}
static int control_epoll(int ep, int operation, int fd, struct epoll_event *event)
{
    return syscall(SYS_epoll_ctl, ep, operation, fd, event);
}
static int wait_epoll(int ep, struct epoll_event *event, int count, int timeout)
{
    return syscall(SYS_epoll_wait, ep, event, count, timeout);
}
static int worker(void *unused)
{
    (void)unused;
    struct epoll_event event;
    for (;;) assert(wait_epoll(epfd, &event, 1, 0) == 1);
}
int main(void)
{
    alarm(30);
    for (int round = 0; round < 100; round++) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            int fds[2];
            assert(pipe(fds) == 0 && write(fds[1], "x", 1) == 1);
            epfd = create_epoll();
            struct epoll_event event = { .events = EPOLLIN, .data.fd = fds[0] };
            assert(epfd >= 0 && control_epoll(epfd, EPOLL_CTL_ADD, fds[0], &event) == 0);
            for (int i = 0; i < 4; i++) {
                size_t n = 64 * 1024;
                char *stack = mmap(NULL, n, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                assert(stack != MAP_FAILED);
                assert(clone(worker, stack + n,
                             CLONE_VM | CLONE_FILES | CLONE_FS |
                             CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM,
                             NULL) >= 0);
            }
            usleep(2000);
            syscall(SYS_exit_group, 0);
            _exit(1);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && status == 0);
        int fds[2], ep = create_epoll();
        assert(ep >= 0 && pipe(fds) == 0 && write(fds[1], "x", 1) == 1);
        struct epoll_event event = { .events = EPOLLIN, .data.fd = fds[0] };
        assert(control_epoll(ep, EPOLL_CTL_ADD, fds[0], &event) == 0);
        assert(wait_epoll(ep, &event, 1, 100) == 1);
        assert(close(ep) == 0);
        close(fds[0]); close(fds[1]);
    }
    puts("epoll group exit: PASS");
    return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
