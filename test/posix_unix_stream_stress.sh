#!/bin/sh
# Concurrent inherited writers exercise ring wrap, partial writes and EOF.
set -eu
BASE=/root/tests/posix_unix_stream_stress
mkdir -p "$BASE"
cat > "$BASE/stress.c" <<'EOF'
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
#define WRITERS 4
#define TOTAL (2 * 1024 * 1024)

static volatile sig_atomic_t notifications;
static void notified(int sig) { (void)sig; ++notifications; }

static void notify_checks(void)
{
    int sv[2], status, flags;
    pid_t child;
    char buffer[4096];
    struct pollfd pfd;
    struct sigaction action;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(sv[0]);
        usleep(50000);
        CHECK(write(sv[1], "x", 1) == 1);
        close(sv[1]);
        _exit(0);
    }
    pfd.fd = sv[0]; pfd.events = POLLIN;
    CHECK(poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLIN));
    CHECK(read(sv[0], buffer, 1) == 1 && buffer[0] == 'x');
    CHECK(waitpid(child, &status, 0) == child && status == 0);

    flags = fcntl(sv[1], F_GETFL);
    CHECK(flags >= 0 && fcntl(sv[1], F_SETFL, flags | O_NONBLOCK) == 0);
    memset(buffer, 'y', sizeof(buffer));
    while (write(sv[1], buffer, sizeof(buffer)) > 0) {}
    CHECK(errno == EAGAIN);
    flags = fcntl(sv[0], F_GETFL);
    CHECK(flags >= 0 && fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(sv[1]);
        usleep(50000);
        while (read(sv[0], buffer, sizeof(buffer)) > 0) {}
        CHECK(errno == EAGAIN);
        close(sv[0]);
        _exit(0);
    }
    pfd.fd = sv[1]; pfd.events = POLLOUT;
    CHECK(poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLOUT));
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    close(sv[0]); close(sv[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    memset(&action, 0, sizeof(action));
    action.sa_handler = notified;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGIO, &action, NULL) == 0);
    CHECK(fcntl(sv[0], F_SETOWN, getpid()) == 0);
    flags = fcntl(sv[0], F_GETFL);
    CHECK(flags >= 0 && fcntl(sv[0], F_SETFL, flags | O_ASYNC) == 0);
    CHECK(write(sv[1], "z", 1) == 1);
    usleep(10000);
    CHECK(notifications > 0);
    CHECK(read(sv[0], buffer, 1) == 1 && buffer[0] == 'z');
    CHECK(fcntl(sv[0], F_SETFL, flags) == 0);
    close(sv[0]); close(sv[1]);
    puts("Unix stream poll and SIGIO notifications PASS");
}

static void run(size_t chunk)
{
    int sv[2], i, status;
    pid_t children[WRITERS];
    unsigned long counts[WRITERS] = {0};
    unsigned char buffer[32768];
    ssize_t n;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    for (i = 0; i < WRITERS; ++i) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (children[i] == 0) {
            unsigned char *payload = malloc(chunk);
            size_t sent = 0;
            alarm(30);
            CHECK(payload != NULL);
            memset(payload, i + 1, chunk);
            close(sv[0]);
            while (sent < TOTAL) {
                size_t size = TOTAL - sent;
                if (size > chunk) size = chunk;
                /* Alternate APIs sharing the stream and ancillary locks. */
                n = i & 1 ? send(sv[1], payload, size, 0) :
                            write(sv[1], payload, size);
                if (n < 0 && errno == EINTR) continue;
                CHECK(n > 0);
                sent += n;
            }
            close(sv[1]);
            _exit(0);
        }
    }
    close(sv[1]);
    while ((n = read(sv[0], buffer, sizeof(buffer))) != 0) {
        ssize_t j;
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0);
        for (j = 0; j < n; ++j) {
            CHECK(buffer[j] >= 1 && buffer[j] <= WRITERS);
            ++counts[buffer[j] - 1];
        }
    }
    close(sv[0]);
    for (i = 0; i < WRITERS; ++i) {
        CHECK(counts[i] == TOTAL);
        CHECK(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    printf("Unix concurrent stream chunk=%lu PASS\n", (unsigned long)chunk);
}

int main(void)
{
    static const size_t chunks[] = {64, 4097, 65536, 262145};
    unsigned i;
    alarm(120);
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i)
        run(chunks[i]);
    notify_checks();
    return 0;
}
EOF
gcc -O2 -Wall -W -o "$BASE/stress" "$BASE/stress.c"
"$BASE/stress"
