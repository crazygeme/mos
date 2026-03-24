/*
 * sock.h — internal declarations for the MOS socket layer.
 *
 * Shared between src/net/sock*.c and src/syscall/syscall_net.c.
 */
#ifndef _NET_SOCK_H
#define _NET_SOCK_H

#include <net/socket.h>
#include <lwip/tcp.h>

/* ── Ring-buffer helpers (sock.c) ───────────────────────────────────────── */

unsigned rx_used(const mos_sock *sk);
unsigned rx_free(const mos_sock *sk);
unsigned rx_write(mos_sock *sk, const void *src, unsigned len);
unsigned rx_read(mos_sock *sk, void *dst, unsigned len);

/* ── Blocking helpers (sock.c) ──────────────────────────────────────────── */

void sock_wakeup(mos_sock *sk);
void sock_wait(mos_sock *sk, unsigned long deadline);

/* ── FD helpers (sock.c) ────────────────────────────────────────────────── */

int sock_to_fd(mos_sock *sk);
mos_sock *fd_to_sock(int fd);

/* ── lwIP callback helpers (sock_cb.c) ──────────────────────────────────── */

void tcp_setup_callbacks(struct tcp_pcb *pcb, mos_sock *sk);
int raw_send_hdrincl(const void *buf, unsigned len);
err_t sock_tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *ip, u16_t port);
void sock_tcp_accept_listen(struct tcp_pcb *pcb, mos_sock *sk);
void sock_udp_recv_setup(struct udp_pcb *pcb, mos_sock *sk);
void sock_raw_recv_setup(struct raw_pcb *pcb, mos_sock *sk);

/* ── Socket operations (sock_ops.c) ─────────────────────────────────────── */

int do_socket(int domain, int type, int protocol);
int do_socketpair(int domain, int type, int protocol, int sv[2]);
int do_bind(int fd, const struct sockaddr_in *addr, unsigned addrlen);
int do_connect(int fd, const struct sockaddr_in *addr, unsigned addrlen);
int do_listen(int fd, int backlog);
int do_accept(int fd, struct sockaddr_in *addr, unsigned *addrlen);
int do_getsockname(int fd, struct sockaddr_in *addr, unsigned *addrlen);
int do_getpeername(int fd, struct sockaddr_in *addr, unsigned *addrlen);
int do_send(int fd, const void *buf, unsigned len, int flags);
int do_recv(int fd, void *buf, unsigned len, int flags);
int do_sendto(int fd, const void *buf, unsigned len, int flags,
	      const struct sockaddr_in *to, unsigned tolen);
int do_recvfrom(int fd, void *buf, unsigned len, int flags,
		struct sockaddr_in *from, unsigned *fromlen);
int do_shutdown(int fd, int how);
int do_setsockopt(int fd, int level, int optname, const void *optval,
		  unsigned optlen);
int do_getsockopt(int fd, int level, int optname, void *optval,
		  unsigned *optlen);

/* ── Message operations (sock_msg.c) ────────────────────────────────────── */

int do_sendmsg(int fd, const struct msghdr *msg, int flags);
int do_recvmsg(int fd, struct msghdr *msg, int flags);

#endif /* _NET_SOCK_H */
