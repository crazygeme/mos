#!/bin/sh
# Validate native signal masks, delivery, and signalfd above signal 32.
set -eu
[ "$(getconf LONG_BIT)" = 64 ] || exit 0
probe_dir=$(mktemp -d /tmp/mos-native-signals.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'C'
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <poll.h>
#include <sched.h>
#include <errno.h>

static volatile sig_atomic_t delivered;
static void handler(int sig) { delivered = sig; }
int main(void)
{
    const int signals[] = { SIGRTMIN, SIGRTMAX };
    for (unsigned i = 0; i < 2; i++) {
        int sig = signals[i];
        sigset_t set, old, current, pending;
        struct sigaction action = { .sa_handler = handler }, previous;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGRTMAX);
        assert(sigaction(sig, &action, &previous) == 0);
        sigemptyset(&set);
        sigaddset(&set, sig);
        assert(sigprocmask(SIG_BLOCK, &set, &old) == 0);
        assert(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
        assert(sigismember(&current, sig) == 1);
        assert(syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig) == 0);
        assert(sigpending(&pending) == 0 && sigismember(&pending, sig) == 1);
        assert(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
        assert(delivered == sig);
        assert(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
        assert(sigismember(&current, SIGRTMAX) == sigismember(&old, SIGRTMAX));
        assert(sigaction(sig, &previous, NULL) == 0);
    }
    cpu_set_t original, selected, queried;
    assert(sched_getaffinity(0, sizeof(original), &original) == 0);
    CPU_ZERO(&selected);
    int cpu = 0;
    while (!CPU_ISSET(cpu, &original)) cpu++;
    CPU_SET(cpu, &selected);
    assert(sched_setaffinity(0, sizeof(selected), &selected) == 0);
    assert(sched_getaffinity(0, sizeof(queried), &queried) == 0);
    assert(CPU_EQUAL(&selected, &queried));
    CPU_ZERO(&queried);
    errno = 0;
    assert(sched_setaffinity(0, sizeof(queried), &queried) == -1 && errno == EINVAL);
    assert(sched_setaffinity(0, sizeof(original), &original) == 0);
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMAX);
    assert(sigprocmask(SIG_BLOCK, &set, &old) == 0);
    struct sigaction action = { .sa_handler = handler };
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    assert(kill(getpid(), SIGUSR1) == 0 && delivered == SIGUSR1);
    sigset_t restored;
    assert(sigprocmask(SIG_SETMASK, NULL, &restored) == 0);
    assert(sigismember(&restored, SIGRTMAX) == 1);
    int fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    assert(fd >= 0);
    assert(kill(getpid(), SIGRTMAX) == 0);
    struct pollfd pollfd = { .fd = fd, .events = POLLIN };
    assert(poll(&pollfd, 1, 1000) == 1);
    struct signalfd_siginfo info;
    assert(read(fd, &info, sizeof(info)) == sizeof(info));
    assert(info.ssi_signo == SIGRTMAX);
    close(fd);
    assert(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
    puts("native realtime signals: PASS");
}
C
gcc -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
