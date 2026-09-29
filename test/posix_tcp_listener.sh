#!/bin/sh
set -eu
BASE=/root/tests/posix_tcp_listener
mkdir -p "$BASE"
cat > "$BASE/listener.c" <<'C'
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #x, errno); exit(1); } } while (0)
static void rejected(int result)
{
    CHECK(result == -1);
    CHECK(errno == ENOTCONN || errno == EPIPE);
}
int main(void)
{
    int listener, client, accepted, val;
    char byte = 'x';
    struct sockaddr_in addr;
    struct pollfd pfd;
    struct iovec iov;
    struct msghdr msg;
    socklen_t len;

    signal(SIGPIPE, SIG_IGN);
    alarm(15);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    len = sizeof(addr);
    CHECK(getsockname(listener, (struct sockaddr *)&addr, &len) == 0);
    CHECK(listen(listener, 4) == 0);
    {
        struct sockaddr_in peer;
        len = sizeof(peer);
        CHECK(getpeername(listener, (struct sockaddr *)&peer, &len) == -1);
        CHECK(errno == ENOTCONN);
    }

    /* A listener must never expose the full PCB's send queue to lwIP. */
    rejected(write(listener, &byte, 1));
    rejected(send(listener, &byte, 1, 0));
    rejected(sendto(listener, &byte, 1, 0, NULL, 0));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    rejected(sendmsg(listener, &msg, 0));
    rejected(writev(listener, &iov, 1));
    rejected(read(listener, &byte, 1));
    rejected(recv(listener, &byte, 1, 0));
    rejected(recvmsg(listener, &msg, 0));
    pfd.fd = listener;
    pfd.events = POLLIN | POLLOUT;
    CHECK(poll(&pfd, 1, 0) == 0);

    /* These options used to read/write past the listener PCB prefix. */
    len = sizeof(val);
    CHECK(getsockopt(listener, SOL_SOCKET, SO_SNDBUF, &val, &len) == 0);
    CHECK(val > 0);
    len = sizeof(val);
    CHECK(getsockopt(listener, IPPROTO_TCP, TCP_MAXSEG, &val, &len) == 0);
    CHECK(val > 0);
    for (val = 0; val <= 1; val++) {
        int actual = -1;
        CHECK(setsockopt(listener, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == 0);
        len = sizeof(actual);
        CHECK(getsockopt(listener, IPPROTO_TCP, TCP_NODELAY, &actual, &len) == 0);
        CHECK(actual == val);
    }

    /* Option changes must preserve the accept callback and normal data I/O. */
    client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client >= 0);
    CHECK(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(poll(&pfd, 1, 1000) == 1);
    CHECK((pfd.revents & POLLIN) && !(pfd.revents & POLLOUT));
    accepted = accept(listener, NULL, NULL);
    CHECK(accepted >= 0);
    len = sizeof(val);
    CHECK(getsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &val, &len) == 0);
    CHECK(val == 1);
    CHECK(write(client, "a", 1) == 1);
    CHECK(read(accepted, &byte, 1) == 1 && byte == 'a');
    CHECK(sendmsg(accepted, &msg, 0) == 1);
    CHECK(recv(client, &byte, 1, 0) == 1 && byte == 'a');
    CHECK(close(accepted) == 0);
    CHECK(close(client) == 0);
    CHECK(close(listener) == 0);
    puts("TCP listener regression passed");
    return 0;
}
C
gcc -Wall -O2 "$BASE/listener.c" -o "$BASE/listener"
"$BASE/listener"
