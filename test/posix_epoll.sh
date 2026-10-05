#!/bin/sh
set -e
BASE=/root/tests/posix_epoll
mkdir -p "$BASE"
cat > "$BASE/epoll_probe.c" <<'EPOLL_SOURCE'
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#ifdef WITH_LIBEVENT
#include <event2/event.h>
#endif

#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #expr, errno); exit(1); } } while (0)
static volatile sig_atomic_t signaled;
static void handler(int sig) { (void)sig; signaled = 1; }
static struct epoll_event ev(uint32_t events, uint64_t data)
{
    struct epoll_event e;
    memset(&e, 0, sizeof(e)); e.events = events; e.data.u64 = data; return e;
}
static void ctl(int ep, int op, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event e = ev(events, data);
    CHECK(epoll_ctl(ep, op, fd, &e) == 0);
}
static void drain(int fd) { char b; CHECK(read(fd, &b, 1) == 1); }
static void reap(pid_t pid) { int status; CHECK(waitpid(pid, &status, 0) == pid); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0); }
static uint64_t ns(void)
{
    struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void basic(void)
{
    int p[2], ep, dupfd, alias, other, reused;
    struct epoll_event out[4], e = ev(EPOLLIN, UINT64_C(0x123456789abcdef0));
    CHECK(sizeof(e) == 12);
    errno = 0; CHECK(epoll_create(0) == -1 && errno == EINVAL);
    errno = 0; CHECK(epoll_create1(1) == -1 && errno == EINVAL);
    ep = epoll_create1(EPOLL_CLOEXEC); CHECK(ep >= 0);
    CHECK(fcntl(ep, F_GETFD) & FD_CLOEXEC);
    CHECK(pipe(p) == 0);
    errno = 0; CHECK(epoll_ctl(-1, EPOLL_CTL_ADD, p[0], &e) == -1 && errno == EBADF);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, -1, &e) == -1 && errno == EBADF);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &e) == -1 && errno == EINVAL);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], NULL) == -1 && errno == EFAULT);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], (void *)1) == -1 && errno == EFAULT);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &e) == -1 && errno == ENOENT);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &e) == 0);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &e) == -1 && errno == EEXIST);
    errno = 0; CHECK(epoll_wait(ep, out, 0, 0) == -1 && errno == EINVAL);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    CHECK(syscall(SYS_epoll_wait, ep, NULL, 1, 0) == 0);
    CHECK(write(p[1], "x", 1) == 1);
    errno = 0; CHECK(syscall(SYS_epoll_wait, ep, (void *)(uintptr_t)-1, 1, 0) == -1 && errno == EFAULT);
    errno = 0; CHECK(syscall(SYS_epoll_wait, ep, (void *)1, 1, 0) == -1 && errno == EFAULT);
    CHECK(epoll_wait(ep, out, 1, 0) == 1 && out[0].events == EPOLLIN && out[0].data.u64 == e.data.u64);
    CHECK(epoll_wait(ep, out, 1, 0) == 1);
    ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLET, 2);
    CHECK(epoll_wait(ep, out, 1, 0) == 1 && out[0].data.u64 == 2);
    CHECK(epoll_wait(ep, out, 1, 0) == 0);
    CHECK(write(p[1], "y", 1) == 1);
    CHECK(epoll_wait(ep, out, 1, 0) == 1);
    drain(p[0]); drain(p[0]); CHECK(write(p[1], "x", 1) == 1);
    CHECK(epoll_wait(ep, out, 1, 0) == 1);
    ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLONESHOT, 3);
    CHECK(epoll_wait(ep, out, 1, 0) == 1 && out[0].data.u64 == 3);
    CHECK(epoll_wait(ep, out, 1, 0) == 0);
    ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLONESHOT, 4);
    CHECK(epoll_wait(ep, out, 1, 0) == 1 && out[0].data.u64 == 4);
    drain(p[0]);
    ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN, 5);
    dupfd = dup(p[0]); CHECK(dupfd >= 0);
    ctl(ep, EPOLL_CTL_ADD, dupfd, EPOLLIN, 6);
    reused = p[0]; CHECK(close(p[0]) == 0);
    CHECK(write(p[1], "x", 1) == 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 2);
    CHECK(out[0].data.u64 != out[1].data.u64);
    drain(dupfd);
    other = epoll_create(1); CHECK(other >= 0);
    CHECK(other == reused);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, other, NULL) == -1 && errno == ENOENT);
    CHECK(close(dupfd) == 0);
    CHECK(epoll_wait(ep, out, 4, 0) == 0);
    CHECK(close(p[1]) == 0); CHECK(close(other) == 0);
    alias = dup(ep); CHECK(alias >= 0); CHECK(close(ep) == 0);
    CHECK(pipe(p) == 0); ctl(alias, EPOLL_CTL_ADD, p[0], 0, 7);
    CHECK(write(p[1], "x", 1) == 1); CHECK(epoll_wait(alias, out, 1, 0) == 0);
    ctl(alias, EPOLL_CTL_MOD, p[0], EPOLLIN, 8);
    CHECK(epoll_wait(alias, out, 1, 0) == 1 && out[0].data.u64 == 8);
    drain(p[0]); CHECK(close(p[1]) == 0);
    CHECK(epoll_wait(alias, out, 1, 0) == 1 && (out[0].events & EPOLLHUP));
    CHECK(epoll_ctl(alias, EPOLL_CTL_DEL, p[0], NULL) == 0);
    CHECK(close(p[0]) == 0); CHECK(close(alias) == 0);
    ep = epoll_create(1); CHECK(ep >= 0);
    other = open("/dev/null", O_RDONLY); CHECK(other >= 0);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, other, &e) == -1 && errno == EPERM);
    close(other); close(ep);
    puts("PASS basic, ET, one-shot, alias lifetime, close cleanup, errors");
}
static void ready_before_add_and_fairness(void)
{
    int ep = epoll_create(1), p[3][2], i;
    unsigned seen = 0;
    struct epoll_event out;
    CHECK(ep >= 0);
    for (i = 0; i < 3; i++) { CHECK(pipe(p[i]) == 0); CHECK(write(p[i][1], "x", 1) == 1); ctl(ep, EPOLL_CTL_ADD, p[i][0], EPOLLIN, i); }
    for (i = 0; i < 3; i++) { CHECK(epoll_wait(ep, &out, 1, 0) == 1); seen |= 1U << out.data.u64; }
    CHECK(seen == 7);
    for (i = 0; i < 3; i++) { drain(p[i][0]); CHECK(write(p[i][1], "x", 1) == 1); }
    for (i = 0; i < 3; i++) { CHECK(epoll_wait(ep, &out, 1, 0) == 1); drain(p[out.data.u64][0]); }
    CHECK(epoll_wait(ep, &out, 1, 0) == 0);
    for (i = 0; i < 3; i++) { close(p[i][0]); close(p[i][1]); } close(ep);
    puts("PASS ready-before-add subscriptions, stale candidates, fairness");
}
static void multiple_and_nesting(void)
{
    int p[2], a = epoll_create(1), b = epoll_create(1), c = epoll_create(1), i, chain[7];
    struct epoll_event out, e = ev(EPOLLIN, 0);
    CHECK(pipe(p) == 0 && a >= 0 && b >= 0 && c >= 0);
    ctl(a, EPOLL_CTL_ADD, p[0], EPOLLIN, 1); ctl(b, EPOLL_CTL_ADD, p[0], EPOLLIN, 2);
    ctl(c, EPOLL_CTL_ADD, a, EPOLLIN | EPOLLET, 3);
    errno = 0; CHECK(epoll_ctl(a, EPOLL_CTL_ADD, c, &e) == -1 && errno == ELOOP);
    CHECK(write(p[1], "x", 1) == 1);
    CHECK(epoll_wait(c, &out, 1, 0) == 1 && out.data.u64 == 3);
    CHECK(epoll_wait(a, &out, 1, 0) == 1 && out.data.u64 == 1);
    CHECK(epoll_wait(b, &out, 1, 0) == 1 && out.data.u64 == 2);
    CHECK(epoll_wait(c, &out, 1, 0) == 0);
    CHECK(write(p[1], "y", 1) == 1);
    CHECK(epoll_wait(c, &out, 1, 0) == 1);
    drain(p[0]); drain(p[0]); CHECK(epoll_wait(c, &out, 1, 0) == 0);
    CHECK(write(p[1], "x", 1) == 1);
    CHECK(epoll_wait(c, &out, 1, 0) == 1);
    close(p[0]); close(p[1]); close(a); close(b); close(c);
    for (i = 0; i < 7; i++) { chain[i] = epoll_create(1); CHECK(chain[i] >= 0); }
    for (i = 0; i < 4; i++) ctl(chain[i], EPOLL_CTL_ADD, chain[i+1], EPOLLIN, i);
    errno = 0; CHECK(epoll_ctl(chain[4], EPOLL_CTL_ADD, chain[5], &e) == -1 && errno == ELOOP);
    for (i = 0; i < 7; i++) close(chain[i]);
    puts("PASS multiple instances, nesting, cycle and depth bounds");
}
static void sockets_and_poll(void)
{
    int ep = epoll_create1(0), s[2];
    struct epoll_event out;
    struct pollfd pf;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    ctl(ep, EPOLL_CTL_ADD, s[0], EPOLLIN | EPOLLRDHUP, 99);
    pf.fd = ep; pf.events = POLLIN; pf.revents = 0;
    CHECK(poll(&pf, 1, 0) == 0);
    CHECK(write(s[1], "x", 1) == 1);
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN));
    CHECK(epoll_wait(ep, &out, 1, 0) == 1 && out.events == EPOLLIN); drain(s[0]);
    CHECK(shutdown(s[1], SHUT_WR) == 0);
    CHECK(epoll_wait(ep, &out, 1, 0) == 1 && (out.events & EPOLLRDHUP));
    close(s[0]); close(s[1]); close(ep);
    puts("PASS UNIX socket readiness, half-close, polling epoll descriptors");
}
static void waits_and_signals(void)
{
    int ep = epoll_create1(0), p[2], gate[2];
    pid_t pid;
    struct epoll_event out;
    struct sigaction sa;
    sigset_t blocked, saved;
    uint64_t start, kernelmask = 0;
    struct { int64_t sec, nsec; } t = {0, 0};
    CHECK(pipe(p) == 0 && pipe(gate) == 0);
    ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 1);
    pid = fork(); CHECK(pid >= 0);
    if (!pid) { close(gate[1]); drain(gate[0]); usleep(50000); CHECK(write(p[1], "x", 1) == 1); drain(gate[0]); _exit(0); }
    close(gate[0]); CHECK(write(gate[1], "x", 1) == 1);
    CHECK(epoll_wait(ep, &out, 1, 2000) == 1);
    drain(p[0]); CHECK(write(gate[1], "x", 1) == 1); reap(pid); close(gate[1]);
    start = ns(); CHECK(epoll_wait(ep, &out, 1, 30) == 0); CHECK(ns()-start >= 20000000);
    CHECK(syscall(SYS_epoll_pwait2, ep, &out, 1, &t, NULL, 8) == 0);
    t.nsec = 1000000000; errno = 0; CHECK(syscall(SYS_epoll_pwait2, ep, &out, 1, &t, NULL, 8) == -1 && errno == EINVAL);
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = handler; sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    ualarm(50000, 0); errno = 0;
    CHECK(epoll_wait(ep, &out, 1, -1) == -1 && errno == EINTR && signaled);
    sigemptyset(&blocked); sigaddset(&blocked, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, &saved) == 0);
    signaled = 0; CHECK(kill(getpid(), SIGUSR1) == 0);
    errno = 0; CHECK(syscall(SYS_epoll_pwait, ep, &out, 1, 1000, &kernelmask, 8) == -1 && errno == EINTR && signaled);
    CHECK(sigprocmask(SIG_SETMASK, NULL, &blocked) == 0 && sigismember(&blocked, SIGUSR1));
    CHECK(sigprocmask(SIG_SETMASK, &saved, NULL) == 0);
    errno = 0; CHECK(syscall(SYS_epoll_pwait, ep, &out, 1, 0, &kernelmask, 4) == -1 && errno == EINVAL);
    close(p[0]); close(p[1]); close(ep);
    puts("PASS blocking wakeup, timeout, EINTR, pwait masks, time64 ABI");
}
static void *wait_thread(void *opaque)
{
    struct epoll_event out;
    int ep = *(int *)opaque;
    CHECK(epoll_wait(ep, &out, 1, 2000) == 1 && out.data.u64 == 42);
    return NULL;
}
static void threaded(void)
{
    int ep = epoll_create1(0), p[2], i;
    pthread_t threads[3];
    CHECK(pipe(p) == 0); ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 42);
    for (i = 0; i < 3; i++) CHECK(pthread_create(&threads[i], NULL, wait_thread, &ep) == 0);
    usleep(50000); CHECK(write(p[1], "x", 1) == 1);
    for (i = 0; i < 3; i++) CHECK(pthread_join(threads[i], NULL) == 0);
    close(p[0]); close(p[1]); close(ep);
    puts("PASS shared-instance concurrent waiters");
}
static void devices_and_tcp(void)
{
    int ep = epoll_create1(0), m, sl, listenfd, client, server;
    struct epoll_event out;
    struct termios term;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    m = posix_openpt(O_RDWR | O_NOCTTY); CHECK(m >= 0);
    CHECK(grantpt(m) == 0 && unlockpt(m) == 0);
    sl = open(ptsname(m), O_RDWR | O_NOCTTY); CHECK(sl >= 0);
    CHECK(tcgetattr(sl, &term) == 0); cfmakeraw(&term); CHECK(tcsetattr(sl, TCSANOW, &term) == 0);
    ctl(ep, EPOLL_CTL_ADD, m, EPOLLIN, 1);
    CHECK(write(sl, "x", 1) == 1); CHECK(epoll_wait(ep, &out, 1, 1000) == 1 && out.data.u64 == 1);
    drain(m); CHECK(epoll_wait(ep, &out, 1, 0) == 0);
    CHECK(write(sl, "x", 1) == 1); CHECK(epoll_wait(ep, &out, 1, 1000) == 1); drain(m);
    close(sl); close(m);
    listenfd = socket(AF_INET, SOCK_STREAM, 0); CHECK(listenfd >= 0);
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listenfd, (void *)&addr, sizeof(addr)) == 0 && listen(listenfd, 2) == 0);
    CHECK(getsockname(listenfd, (void *)&addr, &len) == 0);
    ctl(ep, EPOLL_CTL_ADD, listenfd, EPOLLIN, 2);
    client = socket(AF_INET, SOCK_STREAM, 0); CHECK(client >= 0);
    CHECK(connect(client, (void *)&addr, sizeof(addr)) == 0);
    CHECK(epoll_wait(ep, &out, 1, 1000) == 1 && out.data.u64 == 2);
    server = accept(listenfd, NULL, NULL); CHECK(server >= 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, listenfd, NULL) == 0);
    ctl(ep, EPOLL_CTL_ADD, server, EPOLLIN | EPOLLRDHUP, 3);
    CHECK(write(client, "x", 1) == 1); CHECK(epoll_wait(ep, &out, 1, 1000) == 1 && (out.events & EPOLLIN)); drain(server);
    CHECK(shutdown(client, SHUT_WR) == 0); CHECK(epoll_wait(ep, &out, 1, 1000) == 1 && (out.events & EPOLLRDHUP));
    close(server); close(client); close(listenfd); close(ep);
    puts("PASS PTY subscriptions, TCP listener and stream readiness");
}
static void exit_waiters(void)
{
    int iteration;
    for (iteration = 0; iteration < 8; iteration++) {
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) {
            int ep = epoll_create1(0);
            pthread_t thread;
            CHECK(ep >= 0);
            CHECK(pthread_create(&thread, NULL, wait_thread, &ep) == 0);
            usleep(10000);
            _exit(0);
        }
        reap(pid);
    }
    puts("PASS process exit with blocked epoll waiters");
}
#ifdef WITH_LIBEVENT
static int callbacks;
static void event_read(evutil_socket_t fd, short events, void *opaque)
{
    struct event_base *base = opaque;
    CHECK(events & EV_READ); drain(fd); callbacks++;
    CHECK(event_base_loopbreak(base) == 0);
}
static void libevent_smoke(void)
{
    struct event_config *config = event_config_new();
    struct event_base *base;
    struct event *event;
    int p[2], i;
    CHECK(config != NULL);
    CHECK(event_config_require_features(config, EV_FEATURE_ET) == 0);
    base = event_base_new_with_config(config); CHECK(base != NULL);
    CHECK(strcmp(event_base_get_method(base), "epoll") == 0);
    event_config_free(config);
    CHECK(pipe(p) == 0);
    event = event_new(base, p[0], EV_READ | EV_PERSIST | EV_ET, event_read, base); CHECK(event != NULL);
    CHECK(event_add(event, NULL) == 0);
    for (i = 0; i < 3; i++) { CHECK(write(p[1], "x", 1) == 1); CHECK(event_base_dispatch(base) == 0); }
    CHECK(callbacks == 3);
    event_free(event); event_base_free(base); close(p[0]); close(p[1]);
    puts("PASS libevent epoll backend and persistent edge callbacks");
}
#endif
static void benchmark(void)
{
    const int sizes[] = {1, 64, 256}, loops = 100000;
    int pipes[256][2], ep, i, j, n;
    struct epoll_event out;
    struct pollfd pf[256];
    uint64_t start, empty, ready, scan, control;
    for (j = 0; j < 3; j++) {
        n = sizes[j]; ep = epoll_create1(0); CHECK(ep >= 0);
        for (i = 0; i < n; i++) { CHECK(pipe(pipes[i]) == 0); ctl(ep, EPOLL_CTL_ADD, pipes[i][0], EPOLLIN, i); pf[i].fd = pipes[i][0]; pf[i].events = POLLIN; }
        start = ns(); for (i = 0; i < loops; i++) CHECK(epoll_wait(ep, &out, 1, 0) == 0); empty = ns()-start;
        CHECK(write(pipes[n-1][1], "x", 1) == 1);
        start = ns(); for (i = 0; i < loops; i++) CHECK(epoll_wait(ep, &out, 1, 0) == 1); ready = ns()-start;
        start = ns(); for (i = 0; i < 10000; i++) CHECK(poll(pf, n, 0) == 1); scan = ns()-start;
        start = ns(); for (i = 0; i < 10000; i++) ctl(ep, EPOLL_CTL_MOD, pipes[n-1][0], EPOLLIN, n-1); control = ns()-start;
        printf("BENCH n=%d empty_ns=%llu ready_ns=%llu poll_ns=%llu mod_ns=%llu\n", n, (unsigned long long)(empty/loops), (unsigned long long)(ready/loops), (unsigned long long)(scan/10000), (unsigned long long)(control/10000));
        for (i = 0; i < n; i++) { close(pipes[i][0]); close(pipes[i][1]); } close(ep);
    }
}
int main(int argc, char **argv)
{
    struct utsname u;
    int init = getpid() == 1, logfd;
    (void)argc; (void)argv;
    if (init) {
        mkdir("/proc", 0755); mount("proc", "/proc", "proc", 0, NULL);
        mkdir("/dev/pts", 0755); mount("devpts", "/dev/pts", "devpts", 0, NULL);
        mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
        {
            struct ifreq interface;
            int netfd = socket(AF_INET, SOCK_DGRAM, 0);
            CHECK(netfd >= 0);
            memset(&interface, 0, sizeof(interface));
            strcpy(interface.ifr_name, "lo");
            if (ioctl(netfd, SIOCGIFFLAGS, &interface) == 0) {
                interface.ifr_flags |= IFF_UP;
                (void)ioctl(netfd, SIOCSIFFLAGS, &interface);
            }
            close(netfd);
        }
        logfd = open("/proc/tests/.result", O_WRONLY);
        if (logfd >= 0) {
            const char *suites[] = {"cyclebuf", "SyslogTest"};
            int i;
            dup2(logfd, 1); dup2(logfd, 2); close(logfd);
            for (i = 0; i < 2; i++) {
                int runner = open("/proc/tests/.runner", O_WRONLY);
                CHECK(runner >= 0);
                CHECK(write(runner, suites[i], strlen(suites[i])) == (ssize_t)strlen(suites[i]));
                CHECK(close(runner) == 0);
            }
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    CHECK(uname(&u) == 0); printf("EPOLL_PROBE %s %s %s\n", u.sysname, u.release, u.machine);
    basic(); ready_before_add_and_fairness(); multiple_and_nesting(); sockets_and_poll(); waits_and_signals(); threaded(); devices_and_tcp(); exit_waiters();
#ifdef WITH_LIBEVENT
    libevent_smoke();
#endif
    benchmark();
    puts("EPOLL_PROBE_COMPLETE PASS");
    if (init) { sync(); reboot(RB_POWER_OFF); for (;;) pause(); }
    return 0;
}
EPOLL_SOURCE
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libevent_core; then
    gcc -O2 -Wall -Wextra -Werror -pthread -DWITH_LIBEVENT -o "$BASE/epoll_probe" "$BASE/epoll_probe.c" $(pkg-config --cflags --libs libevent_core)
else
    gcc -O2 -Wall -Wextra -Werror -pthread -o "$BASE/epoll_probe" "$BASE/epoll_probe.c"
fi
"$BASE/epoll_probe"
