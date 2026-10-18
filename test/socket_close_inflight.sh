#!/bin/sh
# Active socket transfers retain their open file descriptions across close().
set -eu
BASE=$(mktemp -d /tmp/mos-socket-close-XXXXXX)
trap 'rm -rf "$BASE"' EXIT HUP INT TERM
cat > "$BASE/check.c" <<'C'
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
struct transfer { int sv[2]; int started[2]; };
static int close_receiver(void *opaque)
{
    struct transfer *t = opaque;
    char c;
    if (read(t->started[0], &c, 1) != 1) return 2;
    usleep(50000);
    if (close(t->sv[0])) return 3;
    if (send(t->sv[1], "K", 1, MSG_NOSIGNAL) != 1) return 4;
    return 0;
}
int main(void)
{
    alarm(20);
    for (int mode = 0; mode < 3; mode++) {
        for (int round = 0; round < 20; round++) {
            struct transfer t;
            char c = 0;
            int status;
            CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, t.sv) == 0);
            CHECK(pipe(t.started) == 0);
            struct timeval timeout = { .tv_sec = 2 };
            CHECK(!setsockopt(t.sv[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
            void *stack = malloc(65536);
            CHECK(stack);
            int child = clone(close_receiver, (char *)stack + 65536,
                              CLONE_FILES | SIGCHLD, &t);
            CHECK(child > 0);
            CHECK(write(t.started[1], "S", 1) == 1);
            struct iovec iov = { .iov_base = &c, .iov_len = 1 };
            struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
            ssize_t n = mode == 0 ? read(t.sv[0], &c, 1) :
                        mode == 1 ? recv(t.sv[0], &c, 1, 0) :
                                    recvmsg(t.sv[0], &msg, 0);
            CHECK(n == 1 && c == 'K');
            CHECK(waitpid(child, &status, 0) == child);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            CHECK(close(t.sv[1]) == 0);
            CHECK(close(t.started[0]) == 0);
            CHECK(close(t.started[1]) == 0);
            free(stack);
        }
    }
    puts("Socket transfers across concurrent close: PASS (60 cases)");
    return 0;
}
C
${CC:-gcc} -O2 -Wall -Werror "$BASE/check.c" -o "$BASE/check"
"$BASE/check"
