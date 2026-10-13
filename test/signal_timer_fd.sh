#!/bin/sh
# Validate signal and timer descriptors through the guest syscall ABI.
set -eu
probe_dir=$(mktemp -d /tmp/mos-signal-timer-fd.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef SI_TKILL
#define SI_TKILL -6
#endif
#define SFD_NONBLOCK O_NONBLOCK
#define SFD_CLOEXEC O_CLOEXEC
#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC O_CLOEXEC
#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2
#define EPOLL_CLOEXEC O_CLOEXEC
#define EPOLLIN 1
#define EPOLLET (1U << 31)
#define EPOLL_CTL_ADD 1
#if defined(__x86_64__)
#define CALL_SIGNALFD4 289
#define CALL_TIMERFD_CREATE 283
#define CALL_TIMERFD_SETTIME 286
#define CALL_TIMERFD_GETTIME 287
#define CALL_EPOLL_CREATE1 291
#define CALL_EPOLL_CTL 233
#define CALL_EPOLL_WAIT 232
#define CALL_TIMER_CREATE 222
#define CALL_TIMER_SETTIME 223
#define CALL_TIMER_DELETE 226
#define CALL_CLOCK_GETTIME 228
#define CALL_TKILL 200
#define CALL_GETTID 186
#else
#define CALL_SIGNALFD4 327
#define CALL_TIMERFD_CREATE 322
#define CALL_TIMERFD_SETTIME 325
#define CALL_TIMERFD_GETTIME 326
#define CALL_EPOLL_CREATE1 329
#define CALL_EPOLL_CTL 255
#define CALL_EPOLL_WAIT 256
#define CALL_TIMER_CREATE 259
#define CALL_TIMER_SETTIME 260
#define CALL_TIMER_DELETE 263
#define CALL_CLOCK_GETTIME 265
#define CALL_TKILL 238
#define CALL_GETTID 224
#endif
struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t ssi_errno, ssi_code;
    uint32_t ssi_pid, ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid, ssi_band, ssi_overrun, ssi_trapno;
    int32_t ssi_status, ssi_int;
    uint64_t ssi_ptr, ssi_utime, ssi_stime, ssi_addr;
    unsigned char padding[48];
};
struct probe_epoll_event {
    uint32_t events;
    union { int fd; uint64_t u64; } data;
} __attribute__((packed));
/* Exercise kernel services independently of guest libc wrapper availability. */
static int signalfd(int fd, const sigset_t *mask, int flags)
{
    uint64_t kernel_mask = 0;
    memcpy(&kernel_mask, mask, sizeof(kernel_mask));
    return syscall(CALL_SIGNALFD4, fd, &kernel_mask, 8, flags);
}
static int timerfd_create(int clockid, int flags)
{
    return syscall(CALL_TIMERFD_CREATE, clockid, flags);
}
static int timerfd_settime(int fd, int flags, const struct itimerspec *value,
                           struct itimerspec *old)
{
    return syscall(CALL_TIMERFD_SETTIME, fd, flags, value, old);
}
static int timerfd_gettime(int fd, struct itimerspec *value)
{
    return syscall(CALL_TIMERFD_GETTIME, fd, value);
}
static int probe_epoll_create1(int flags)
{
    return syscall(CALL_EPOLL_CREATE1, flags);
}
static int probe_epoll_ctl(int ep, int op, int fd, struct probe_epoll_event *value)
{
    return syscall(CALL_EPOLL_CTL, ep, op, fd, value);
}
static int probe_epoll_wait(int ep, struct probe_epoll_event *value, int count, int timeout)
{
    return syscall(CALL_EPOLL_WAIT, ep, value, count, timeout);
}
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); \
    exit(1); } } while (0)
static void child_ok(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static uint64_t ticks(int fd)
{
    uint64_t value;
    CHECK(read(fd, &value, sizeof(value)) == sizeof(value));
    return value;
}
static void ready(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN));
}
static int watch(int fd)
{
    int ep = probe_epoll_create1(EPOLL_CLOEXEC);
    struct probe_epoll_event event = { .events = EPOLLIN | EPOLLET, .data.fd = fd };
    CHECK(ep >= 0 && probe_epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == 0);
    return ep;
}
static void ep_ready(int ep, int fd)
{
    struct probe_epoll_event event;
    CHECK(probe_epoll_wait(ep, &event, 1, 2000) == 1);
    CHECK(event.data.fd == fd && (event.events & EPOLLIN));
}
static void no_data(int fd)
{
    char buf[128];
    CHECK(read(fd, buf, sizeof(buf)) == -1 && errno == EAGAIN);
}
static void signals(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGCHLD);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
    int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    CHECK(fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    no_data(fd);
    char small[127];
    CHECK(read(fd, small, sizeof(small)) == -1 && errno == EINVAL);
    int ep = watch(fd);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        usleep(50000);
        CHECK(kill(getppid(), SIGUSR1) == 0);
        usleep(150000);
        _exit(7);
    }
    ep_ready(ep, fd);
    struct signalfd_siginfo info[3];
    CHECK(read(fd, info, sizeof(*info)) == sizeof(*info));
    CHECK(info[0].ssi_signo == SIGUSR1 && info[0].ssi_code == SI_USER);
    CHECK(info[0].ssi_pid == (unsigned)child && info[0].ssi_uid == getuid());
    ready(fd);
    CHECK(read(fd, info, sizeof(*info)) == sizeof(*info));
    CHECK(info[0].ssi_signo == SIGCHLD && info[0].ssi_code == CLD_EXITED);
    CHECK(info[0].ssi_pid == (unsigned)child && info[0].ssi_status == 7);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WEXITSTATUS(status) == 7);
    no_data(fd);
    union sigval value = { .sival_int = 0x1234 };
    CHECK(sigqueue(getpid(), SIGUSR2, value) == 0);
    CHECK(kill(getpid(), SIGUSR1) == 0);
    CHECK(read(fd, info, sizeof(info)) == 2 * sizeof(*info));
    CHECK(info[0].ssi_signo == SIGUSR1 && info[1].ssi_signo == SIGUSR2);
    CHECK(info[1].ssi_code == SI_QUEUE && info[1].ssi_int == value.sival_int);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGKILL);
    sigaddset(&mask, SIGSTOP);
    CHECK(signalfd(fd, &mask, 0) == fd);
    CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
    CHECK(kill(getpid(), SIGUSR1) == 0);
    no_data(fd);
    CHECK(kill(getpid(), SIGUSR2) == 0);
    CHECK(read(fd, info, sizeof(*info)) == sizeof(*info));
    CHECK(info[0].ssi_signo == SIGUSR2);
    sigaddset(&mask, SIGUSR1);
    CHECK(signalfd(fd, &mask, 0) == fd);
    ep_ready(ep, fd);
    CHECK(read(fd, info, sizeof(*info)) == sizeof(*info));
    CHECK(info[0].ssi_signo == SIGUSR1);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(kill(getpid(), SIGUSR1) == 0);
        CHECK(read(fd, info, sizeof(*info)) == sizeof(*info));
        CHECK(info[0].ssi_pid == (unsigned)getpid());
        _exit(0);
    }
    child_ok(child);
    no_data(fd);
    close(ep);
    close(fd);
    /* A blocking read must wake for a blocked signal. */
    fd = signalfd(-1, &mask, 0);
    CHECK(fd >= 0);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        usleep(50000);
        CHECK(kill(getppid(), SIGUSR1) == 0);
        _exit(0);
    }
    CHECK(read(fd, info, sizeof(*info)) == sizeof(*info));
    CHECK(info[0].ssi_signo == SIGUSR1);
    child_ok(child);
    close(fd);
}
static int thread_fd, thread_ready[2], thread_continue[2];
static void *thread_signal(void *unused)
{
    int ep = watch(thread_fd);
    CHECK(syscall(CALL_TKILL, syscall(CALL_GETTID), SIGUSR2) == 0);
    char byte = 'x';
    CHECK(write(thread_ready[1], &byte, 1) == 1);
    CHECK(read(thread_continue[0], &byte, 1) == 1);
    ep_ready(ep, thread_fd);
    struct signalfd_siginfo info;
    CHECK(read(thread_fd, &info, sizeof(info)) == sizeof(info));
    CHECK(info.ssi_signo == SIGUSR2 && info.ssi_code == SI_TKILL);
    close(ep);
    return NULL;
}
static void thread_signals(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    thread_fd = signalfd(-1, &mask, SFD_NONBLOCK);
    CHECK(thread_fd >= 0);
    CHECK(pipe(thread_ready) == 0 && pipe(thread_continue) == 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, thread_signal, NULL) == 0);
    char byte;
    CHECK(read(thread_ready[0], &byte, 1) == 1);
    no_data(thread_fd);
    struct pollfd p = { .fd = thread_fd, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(write(thread_continue[1], &byte, 1) == 1);
    CHECK(pthread_join(thread, NULL) == 0);
    close(thread_ready[0]); close(thread_ready[1]);
    close(thread_continue[0]); close(thread_continue[1]);
    close(thread_fd);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    int fd = signalfd(-1, &mask, SFD_NONBLOCK);
    CHECK(fd >= 0);
    struct sigevent event = { .sigev_notify = SIGEV_SIGNAL,
                             .sigev_signo = SIGUSR1,
                             .sigev_value.sival_int = 0x4321 };
    int timer;
    CHECK(syscall(CALL_TIMER_CREATE, CLOCK_MONOTONIC, &event, &timer) == 0);
    struct itimerspec value = { .it_value.tv_nsec = 30000000 };
    CHECK(syscall(CALL_TIMER_SETTIME, timer, 0, &value, NULL) == 0);
    ready(fd);
    struct signalfd_siginfo info;
    CHECK(read(fd, &info, sizeof(info)) == sizeof(info));
    CHECK(info.ssi_code == SI_TIMER && info.ssi_int == 0x4321);
    CHECK(syscall(CALL_TIMER_DELETE, timer) == 0);
    close(fd);
}
static void timers(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    CHECK(fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    no_data(fd);
    char small[7];
    CHECK(read(fd, small, sizeof(small)) == -1 && errno == EINVAL);
    struct itimerspec value = { .it_value.tv_nsec = 50000000 }, old;
    int ep = watch(fd);
    CHECK(timerfd_settime(fd, 0, &value, &old) == 0);
    CHECK(old.it_value.tv_sec == 0 && old.it_value.tv_nsec == 0);
    ep_ready(ep, fd);
    CHECK(ticks(fd) == 1);
    no_data(fd);
    CHECK(timerfd_gettime(fd, &old) == 0 && !old.it_value.tv_sec && !old.it_value.tv_nsec);
    value.it_interval.tv_nsec = 10000000;
    value.it_value.tv_nsec = 10000000;
    CHECK(timerfd_settime(fd, 0, &value, NULL) == 0);
    usleep(85000);
    CHECK(ticks(fd) >= 5);
    CHECK(timerfd_gettime(fd, &old) == 0 && old.it_interval.tv_nsec == 10000000);
    memset(&value, 0, sizeof(value));
    CHECK(timerfd_settime(fd, 0, &value, &old) == 0);
    CHECK(old.it_interval.tv_nsec == 10000000);
    no_data(fd);
    struct timespec now;
    CHECK(syscall(CALL_CLOCK_GETTIME, CLOCK_MONOTONIC, &now) == 0);
    value.it_value = now;
    CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME, &value, NULL) == 0);
    CHECK(ticks(fd) == 1);
    int alias = dup(fd);
    CHECK(alias >= 0);
    value.it_value.tv_sec = 0;
    value.it_value.tv_nsec = 50000000;
    CHECK(timerfd_settime(fd, 0, &value, NULL) == 0);
    close(fd);
    ready(alias);
    CHECK(ticks(alias) == 1);
    close(alias);
    close(ep);
    fd = timerfd_create(CLOCK_BOOTTIME, 0);
    CHECK(fd >= 0 && timerfd_settime(fd, 0, &value, NULL) == 0);
    CHECK(ticks(fd) == 1);
    close(fd);
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    CHECK(fd >= 0 && timerfd_settime(fd, 0, &value, NULL) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        ready(fd);
        CHECK(ticks(fd) == 1);
        _exit(0);
    }
    child_ok(child);
    no_data(fd);
    value.it_value.tv_nsec = 1000000000;
    CHECK(timerfd_settime(fd, 0, &value, NULL) == -1 && errno == EINVAL);
    CHECK(timerfd_settime(fd, 4, &value, NULL) == -1 && errno == EINVAL);
    CHECK(timerfd_gettime(-1, &old) == -1 && errno == EBADF);
    CHECK(timerfd_settime(fd, 0, NULL, NULL) == -1 && errno == EFAULT);
    close(fd);
    CHECK(timerfd_create(CLOCK_PROCESS_CPUTIME_ID, 0) == -1 && errno == EINVAL);
    CHECK(timerfd_create(CLOCK_MONOTONIC, 1) == -1 && errno == EINVAL);
#if defined(__i386__)
    struct { int64_t is, in, vs, vn; } wide = { 0x80000000LL, 0, 1, 0 }, result;
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    CHECK(fd >= 0);
    CHECK(syscall(411, fd, 0, &wide, NULL) == 0);
    CHECK(syscall(410, fd, &result) == 0 && result.is == wide.is);
    CHECK(syscall(326, fd, &old) == -1 && errno == EOVERFLOW);
    close(fd);
#endif
    /* An absolute realtime timer must report discontinuous clock changes. */
    if (geteuid() == 0) {
        struct timeval wall;
        CHECK(gettimeofday(&wall, NULL) == 0);
        fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
        CHECK(fd >= 0);
        value = (struct itimerspec){ .it_value.tv_sec = wall.tv_sec + 60 };
        CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
                              &value, NULL) == 0);
        struct timeval changed = wall;
        changed.tv_sec++;
        int changed_ok = settimeofday(&changed, NULL);
        if (changed_ok == 0) {
            uint64_t count;
            int ret = read(fd, &count, sizeof(count));
            int error = errno;
            CHECK(settimeofday(&wall, NULL) == 0);
            CHECK(ret == -1 && error == ECANCELED);
        } else {
            CHECK(errno == EPERM);
        }
        close(fd);
    }
}
int main(void)
{
    signals();
    thread_signals();
    timers();
    return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
