#!/bin/sh
# Validate TCP PCB ownership across shutdown, descriptor reuse, and close.
set -eu
probe_dir=$(mktemp -d /tmp/mos-tcp-shutdown.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'C'
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#ifndef EPOLLRDHUP
#define EPOLLRDHUP 0x2000
#endif
#ifdef __i386__
#define CALL_EPOLL_CREATE1 329
#define CALL_EPOLL_CTL 255
#define CALL_EPOLL_WAIT 256
#else
#define CALL_EPOLL_CREATE1 291
#define CALL_EPOLL_CTL 233
#define CALL_EPOLL_WAIT 232
#endif
/* The guest libc has epoll types but no syscall wrappers. */
#define epoll_create1(f) syscall(CALL_EPOLL_CREATE1, (f))
#define epoll_ctl(e, op, fd, ev) syscall(CALL_EPOLL_CTL, (e), (op), (fd), (ev))
#define epoll_wait(e, ev, n, t) syscall(CALL_EPOLL_WAIT, (e), (ev), (n), (t))
#define CHECK(x) do { if (!(x)) { \
 fprintf(stderr, "%d: %s (errno=%d)\n", __LINE__, #x, errno); exit(1); \
} } while (0)
static int listener;
static struct sockaddr_in address;
static void pair(int *client, int *server)
{
 *client = socket(AF_INET, SOCK_STREAM, 0);
 CHECK(*client >= 0);
 CHECK(connect(*client, (struct sockaddr *)&address, sizeof(address)) == 0);
 *server = accept(listener, NULL, NULL);
 CHECK(*server >= 0);
}
static void echo(int client, int server)
{
 char c;
 CHECK(send(client, "X", 1, MSG_NOSIGNAL) == 1);
 CHECK(recv(server, &c, 1, 0) == 1 && c == 'X');
 CHECK(send(server, "Y", 1, MSG_NOSIGNAL) == 1);
 CHECK(recv(client, &c, 1, 0) == 1 && c == 'Y');
}
static void ended_write(int fd)
{
 char c = 'X';
 struct iovec iov = {&c, 1};
 struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
 CHECK(send(fd, &c, 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
 CHECK(sendmsg(fd, &msg, MSG_NOSIGNAL) == -1 && errno == EPIPE);
 CHECK(write(fd, &c, 1) == -1 && errno == EPIPE);
}
int main(int argc, char **argv)
{
 int c, s, n, m, stale, fd, status, rounds = argc > 1 ? atoi(argv[1]) : 240;
 unsigned length = sizeof(address);
 char byte;
 signal(SIGPIPE, SIG_IGN);
 alarm(120);
 fd = socket(AF_INET, SOCK_STREAM, 0);
 CHECK(fd >= 0);
 CHECK(shutdown(fd, SHUT_RDWR) == -1 && errno == ENOTCONN);
 CHECK(close(fd) == 0);
 listener = socket(AF_INET, SOCK_STREAM, 0);
 CHECK(listener >= 0);
 address.sin_family = AF_INET;
 address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
 CHECK(listen(listener, 8) == 0);
 CHECK(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
 CHECK(shutdown(listener, SHUT_RDWR) == -1 && errno == ENOTCONN);
 pair(&c, &s);
 CHECK(shutdown(c, SHUT_WR) == 0 && shutdown(c, SHUT_WR) == 0);
 ended_write(c);
 CHECK(recv(s, &byte, 1, 0) == 0);
 CHECK(send(s, "R", 1, MSG_NOSIGNAL) == 1);
 CHECK(recv(c, &byte, 1, 0) == 1 && byte == 'R');
 CHECK(shutdown(s, SHUT_WR) == 0);
 CHECK(recv(c, &byte, 1, 0) == 0);
 CHECK(shutdown(c, SHUT_RDWR) == 0);
 CHECK(close(c) == 0 && close(s) == 0);
 pair(&c, &s);
 pid_t child = fork();
 CHECK(child >= 0);
 if (!child) {
  CHECK(recv(c, &byte, 1, 0) == 0);
  _exit(0);
 }
 usleep(20000);
 CHECK(shutdown(c, SHUT_RD) == 0);
 CHECK(waitpid(child, &status, 0) == child);
 CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
 CHECK(send(c, "W", 1, MSG_NOSIGNAL) == 1);
 CHECK(recv(s, &byte, 1, 0) == 1 && byte == 'W');
 CHECK(shutdown(c, SHUT_WR) == 0);
 CHECK(close(c) == 0 && close(s) == 0);
 for (int i = 0; i < rounds; ++i) {
  pair(&stale, &s);
  CHECK(shutdown(s, SHUT_WR) == 0);
  CHECK(recv(stale, &byte, 1, 0) == 0);
  if (i % 3 == 0) CHECK(shutdown(stale, SHUT_RDWR) == 0);
  if (i % 3 == 1) {
   CHECK(shutdown(stale, SHUT_RD) == 0);
   CHECK(shutdown(stale, SHUT_WR) == 0);
  }
  if (i % 3 == 2) {
   CHECK(shutdown(stale, SHUT_WR) == 0);
   CHECK(shutdown(stale, SHUT_RD) == 0);
  }
  CHECK(shutdown(stale, SHUT_RDWR) == 0);
  ended_write(stale);
  CHECK(recv(stale, &byte, 1, MSG_DONTWAIT) == 0);
  struct pollfd p = {.fd = stale, .events = POLLIN | POLLOUT};
  CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN) &&
        (p.revents & POLLOUT) && (p.revents & POLLHUP));
  int ep = epoll_create1(0);
  struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP};
  CHECK(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, stale, &ev) == 0);
  CHECK(epoll_wait(ep, &ev, 1, 0) == 1 && (ev.events & EPOLLIN) &&
        (ev.events & EPOLLRDHUP) && (ev.events & EPOLLHUP));
  CHECK(close(ep) == 0);
  CHECK(close(s) == 0);
  /* FIN acknowledgements release the PCB while its socket remains open. */
  usleep(2000);
  pair(&c, &s);
  pair(&n, &m);
  CHECK(close(stale) == 0);
  echo(c, s);
  echo(n, m);
  CHECK(close(c) == 0 && close(s) == 0);
  CHECK(close(n) == 0 && close(m) == 0);
 }
 /* Keep a fully half-closed descriptor beyond TIME_WAIT pool reclamation. */
 pair(&c, &stale);
 CHECK(shutdown(stale, SHUT_WR) == 0);
 CHECK(recv(c, &byte, 1, 0) == 0);
 CHECK(shutdown(c, SHUT_WR) == 0);
 CHECK(recv(stale, &byte, 1, 0) == 0);
 CHECK(close(c) == 0);
 for (int i = 0; i < 160; ++i) {
  pair(&c, &s);
  echo(c, s);
  CHECK(shutdown(c, SHUT_RDWR) == 0);
  CHECK(close(c) == 0 && close(s) == 0);
 }
 CHECK(close(stale) == 0);
 pair(&c, &s);
 pair(&n, &m);
 echo(c, s);
 echo(n, m);
 CHECK(close(c) == 0 && close(s) == 0);
 CHECK(close(n) == 0 && close(m) == 0);
 CHECK(close(listener) == 0);
 printf("tcp_shutdown_lifecycle: PASS (%d reuse rounds, 160 TIME_WAIT reclamations)\n", rounds);
 return 0;
}
C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe" "$@"
