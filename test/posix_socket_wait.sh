#!/bin/sh
# Fast checks run by default. Set MOS_SOCKET_LONG_WAIT=1 explicitly to add
# the three 32-second regressions for the former hard-coded receive timeout.
set -eu
BASE=/root/tests/posix_socket_wait
mkdir -p "$BASE"
cat > "$BASE/wait.c" <<'EOF'
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #x, errno); exit(1); } } while (0)
static volatile sig_atomic_t interrupted;
static void handler(int sig) { (void)sig; interrupted++; }
/* The test runner captures stdout/stderr until completion. Send progress
 * directly to its result channel so a blocked phase remains identifiable. */
static void progress(const char *phase, int domain, int type, int mode)
{
    char line[192];
    int fd, len;
    len = snprintf(line, sizeof(line),
        "posix_socket_wait: pid=%ld phase=%s domain=%d type=%d mode=%d\n",
        (long)getpid(), phase, domain, type, mode);
    fd = open("/proc/tests/.result", O_WRONLY);
    if (fd >= 0) {
        write(fd, line, len);
        close(fd);
    } else {
        fputs(line, stderr);
        fflush(stderr);
    }
}
static long long now_ms(void)
{
    struct timeval tv;
    CHECK(gettimeofday(&tv, NULL) == 0);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static void timeout_set(int fd, int option, int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    CHECK(setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv)) == 0);
}
static int receive(int fd, int mode, char *c)
{
    struct iovec iov;
    struct msghdr msg;
    if (mode == 0) return read(fd, c, 1);
    if (mode == 1) return recv(fd, c, 1, 0);
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = c; iov.iov_len = 1;
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    return recvmsg(fd, &msg, 0);
}
static int listener(int domain, struct sockaddr_storage *addr, socklen_t *len)
{
    int fd = socket(domain, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(addr, 0, sizeof(*addr));
    if (domain == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *)addr;
        un->sun_family = domain;
        snprintf(un->sun_path, sizeof(un->sun_path), "/tmp/socket-wait-%ld", (long)getpid());
        unlink(un->sun_path);
        *len = sizeof(*un);
    } else {
        struct sockaddr_in *in = (struct sockaddr_in *)addr;
        in->sin_family = domain; in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        *len = sizeof(*in);
    }
    CHECK(bind(fd, (struct sockaddr *)addr, *len) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)addr, len) == 0);
    CHECK(listen(fd, 4) == 0);
    return fd;
}
static void pair(int domain, int type, int sv[2])
{
    struct sockaddr_storage addr;
    socklen_t len;
    int lfd;
    if (domain == AF_UNIX) { CHECK(socketpair(domain, type, 0, sv) == 0); return; }
    if (type == SOCK_STREAM) {
        lfd = listener(domain, &addr, &len);
        sv[0] = socket(domain, type, 0); CHECK(sv[0] >= 0);
        CHECK(connect(sv[0], (struct sockaddr *)&addr, len) == 0);
        sv[1] = accept(lfd, NULL, NULL); CHECK(sv[1] >= 0); close(lfd);
    } else {
        int i;
        for (i = 0; i < 2; i++) {
            struct sockaddr_in in;
            sv[i] = socket(domain, type, 0); CHECK(sv[i] >= 0);
            memset(&in, 0, sizeof(in)); in.sin_family = domain;
            in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            CHECK(bind(sv[i], (struct sockaddr *)&in, sizeof(in)) == 0);
        }
        for (i = 0; i < 2; i++) {
            len = sizeof(addr);
            CHECK(getsockname(sv[1-i], (struct sockaddr *)&addr, &len) == 0);
            CHECK(connect(sv[i], (struct sockaddr *)&addr, len) == 0);
        }
    }
}
static void wait_child(pid_t pid)
{
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void endpoint(int domain, int type)
{
    int sv[2], duplicate, on, mode, original;
    char c;
    struct sigaction sa;
    pair(domain, type, sv);
    original = fcntl(sv[0], F_GETFL); CHECK(original >= 0);
    duplicate = dup(sv[0]); CHECK(duplicate >= 0);
    on = 1; CHECK(ioctl(duplicate, FIONBIO, &on) == 0);
    CHECK(fcntl(sv[0], F_GETFL) == (original | O_NONBLOCK));
    for (mode = 0; mode < 3; mode++) {
        long long start = now_ms();
        CHECK(receive(sv[0], mode, &c) == -1 && errno == EAGAIN);
        CHECK(now_ms() - start < 1000);
    }
    on = 0; CHECK(ioctl(duplicate, FIONBIO, &on) == 0);
    CHECK(fcntl(sv[0], F_GETFL) == (original & ~O_NONBLOCK));
    close(duplicate);
    timeout_set(sv[0], SO_RCVTIMEO, 150);
    for (mode = 0; mode < 3; mode++) {
        long long start = now_ms();
        CHECK(receive(sv[0], mode, &c) == -1 && errno == EAGAIN);
        CHECK(now_ms() - start >= 100 && now_ms() - start < 2000);
    }
    timeout_set(sv[0], SO_RCVTIMEO, 0);
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask); CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
    progress("alarm-interrupt", domain, type, 0);
    interrupted = 0; alarm(1);
    CHECK(receive(sv[0], 0, &c) == -1 && errno == EINTR && interrupted);
    alarm(0);
    CHECK(write(sv[1], "x", 1) == 1);
    CHECK(read(sv[0], &c, 1) == 1 && c == 'x');
    close(sv[0]); close(sv[1]);
}
/* ITIMER_REAL must also fire while blocked, and respect masking/disarming. */
static void interval_alarm(void)
{
    int sv[2], mode;
    char c;
    struct itimerval timer, remaining;
    struct pollfd pfd;
    sigset_t mask, oldmask, pending;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = timer.it_interval.tv_usec = 150000;
    interrupted = 0;
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    for (mode = 0; mode < 3; mode++) {
        CHECK(receive(sv[0], mode, &c) == -1 && errno == EINTR);
        CHECK(interrupted == mode + 1);
    }
    memset(&timer, 0, sizeof(timer));
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    pfd.fd = sv[0]; pfd.events = POLLIN;
    CHECK(poll(&pfd, 1, 300) == 0);
    CHECK(interrupted == 3);

    sigemptyset(&mask); sigaddset(&mask, SIGALRM);
    CHECK(sigprocmask(SIG_BLOCK, &mask, &oldmask) == 0);
    timer.it_value.tv_usec = 150000;
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    CHECK(poll(&pfd, 1, 300) == 0);
    CHECK(interrupted == 3);
    CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGALRM));
    CHECK(getitimer(ITIMER_REAL, &remaining) == 0);
    CHECK(remaining.it_value.tv_sec == 0 && remaining.it_value.tv_usec == 0);
    CHECK(sigprocmask(SIG_SETMASK, &oldmask, NULL) == 0);
    CHECK(interrupted == 4);
    close(sv[0]); close(sv[1]);
}
static void ignored_signal_wait(void)
{
    int sv[2];
    pid_t pid;
    char c;
    struct sigaction sa, oldsa;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, &oldsa) == 0);
    pid = fork(); CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]); usleep(150000);
        CHECK(kill(getppid(), SIGUSR1) == 0);
        usleep(150000);
        CHECK(write(sv[1], "I", 1) == 1);
        close(sv[1]); _exit(0);
    }
    close(sv[1]);
    CHECK(read(sv[0], &c, 1) == 1 && c == 'I');
    close(sv[0]); wait_child(pid);
    CHECK(sigaction(SIGUSR1, &oldsa, NULL) == 0);
}
static void send_timeout(void)
{
    int sv[2], on = 1, mode;
    char buf[4096];
    memset(buf, 'a', sizeof(buf));
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(ioctl(sv[0], FIONBIO, &on) == 0);
    while (write(sv[0], buf, sizeof(buf)) > 0) {}
    CHECK(errno == EAGAIN);
    on = 0; CHECK(ioctl(sv[0], FIONBIO, &on) == 0);
    timeout_set(sv[0], SO_SNDTIMEO, 150);
    for (mode = 0; mode < 3; mode++) {
        int ret;
        long long start = now_ms();
        struct iovec iov;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buf; iov.iov_len = 1;
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        ret = mode == 0 ? write(sv[0], buf, 1) : mode == 1 ? send(sv[0], buf, 1, 0) : sendmsg(sv[0], &msg, 0);
        CHECK(ret == -1 && errno == EAGAIN);
        CHECK(now_ms() - start >= 100 && now_ms() - start < 2000);
    }
    close(sv[0]); close(sv[1]);
}
static void accept_timeout(int domain)
{
    struct sockaddr_storage addr;
    socklen_t len;
    int fd = listener(domain, &addr, &len), on = 1;
    long long start;
    CHECK(ioctl(fd, FIONBIO, &on) == 0);
    CHECK(accept(fd, NULL, NULL) == -1 && errno == EAGAIN);
    on = 0; CHECK(ioctl(fd, FIONBIO, &on) == 0);
    timeout_set(fd, SO_RCVTIMEO, 150);
    start = now_ms();
    CHECK(accept(fd, NULL, NULL) == -1 && errno == EAGAIN);
    CHECK(now_ms() - start >= 100 && now_ms() - start < 2000);
    close(fd);
    if (domain == AF_UNIX) unlink(((struct sockaddr_un *)&addr)->sun_path);
}
/* Exceed the old hard-coded 30 second limit in parallel for every transport. */
static void long_wait(int domain, int type, int mode)
{
    int sv[2];
    pid_t pid;
    char c;
    progress("long-wait-connect", domain, type, mode);
    pair(domain, type, sv);
    /* Also verify that setting a timeout back to zero restores unlimited wait. */
    timeout_set(sv[0], SO_RCVTIMEO, 150);
    timeout_set(sv[0], SO_RCVTIMEO, 0);
    pid = fork(); CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]); sleep(32);
        progress("long-wait-send", domain, type, mode);
        CHECK(write(sv[1], "L", 1) == 1); close(sv[1]); _exit(0);
    }
    close(sv[1]);
    alarm(45);
    progress("long-wait-receive-32s", domain, type, mode);
    CHECK(receive(sv[0], mode, &c) == 1 && c == 'L');
    alarm(0); close(sv[0]); wait_child(pid);
    progress("long-wait-done", domain, type, mode);
}
int main(void)
{
    int d, t, mode, n;
    pid_t children[4];
    const char *long_wait_env = getenv("MOS_SOCKET_LONG_WAIT");
    signal(SIGPIPE, SIG_IGN);
    alarm(90);
    for (d = 0; d < 2; d++) {
        progress("accept-timeout", d ? AF_INET : AF_UNIX, SOCK_STREAM, -1);
        accept_timeout(d ? AF_INET : AF_UNIX);
        for (t = 0; t < 2; t++) {
            progress("endpoint", d ? AF_INET : AF_UNIX, t ? SOCK_DGRAM : SOCK_STREAM, -1);
            endpoint(d ? AF_INET : AF_UNIX, t ? SOCK_DGRAM : SOCK_STREAM);
        }
    }
    progress("send-timeout", AF_UNIX, SOCK_STREAM, -1);
    send_timeout();
    progress("interval-alarm", AF_UNIX, SOCK_STREAM, -1);
    interval_alarm();
    progress("ignored-signal", AF_UNIX, SOCK_STREAM, -1);
    ignored_signal_wait();
    if (long_wait_env && strcmp(long_wait_env, "1") == 0) {
        /* One pair per transport at a time fits lwIP's default PCB pools. */
        for (mode = 0; mode < 3; mode++) {
            progress("long-wait-batch-32s", 0, 0, mode);
            n = 0;
            for (d = 0; d < 2; d++) for (t = 0; t < 2; t++) {
                pid_t pid = fork(); CHECK(pid >= 0);
                if (pid == 0) { long_wait(d ? AF_INET : AF_UNIX, t ? SOCK_DGRAM : SOCK_STREAM, mode); _exit(0); }
                children[n++] = pid;
            }
            for (d = 0; d < n; d++) wait_child(children[d]);
        }
    }
    progress("complete", 0, 0, -1);
    puts("socket wait regressions passed");
    return 0;
}
EOF
gcc -Wall -Werror -o "$BASE/wait" "$BASE/wait.c"
echo 'posix_socket_wait: phase=compiled' > /proc/tests/.result
"$BASE/wait"
