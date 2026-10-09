/*
 * sock.h — internal declarations for the MOS socket layer.
 *
 * Shared between src/net/sock*.c and src/syscall/syscall_net.c.
 */
#ifndef _NET_SOCK_H
#define _NET_SOCK_H

#include <fs/fs.h>
#include <net/socket.h>
#include <lwip/tcp.h>

/* Only the common PCB prefix is valid on lwIP's smaller listen allocation. */
static inline int sock_tcp_has_full_pcb(const mos_sock *sk)
{
	return sk->domain == AF_INET && sk->type == SOCK_STREAM && sk->tcp &&
	       sk->tcp->state != LISTEN;
}

/* ── Ring-buffer helpers (sock.c) ───────────────────────────────────────── */

unsigned rx_used(const mos_sock *sk);
unsigned rx_free(const mos_sock *sk);
unsigned rx_write(mos_sock *sk, const void *src, unsigned len);
unsigned rx_read(mos_sock *sk, void *dst, unsigned len);
unsigned rx_iov_write(mos_sock *sk, const struct iovec *iov, size_t iovlen);
unsigned rx_iov_read(mos_sock *sk, const struct iovec *iov, size_t iovlen,
		     unsigned limit);
void rx_discard(mos_sock *sk, unsigned len);
int sock_alloc_rxbuf(mos_sock *sk, unsigned size);
void sock_destroy(mos_sock *sk);
unsigned sock_default_rxbuf_size(int domain, int type);
/* Zero deadline means no application-specified timeout. */
unsigned long long sock_recv_deadline(const mos_sock *sk);
unsigned long long sock_send_deadline(const mos_sock *sk);
int sock_deadline_expired(unsigned long long deadline);

/* ── Blocking helpers (sock.c) ──────────────────────────────────────────── */

/* Unix callers must own net_core_lock. Wait enrollment precedes the
 * ownership release in sock_wait; sock_wakeup relies on that ordering. */
void sock_wakeup(mos_sock *sk);
/* Zero deadline waits indefinitely. Returns -1 for a deliverable signal. */
int sock_wait(mos_sock *sk, unsigned long long deadline);

/* ── FD helpers (sock.c) ────────────────────────────────────────────────── */

ssize_t sock_recvmsg_file_iov(file *fp, const struct iovec *iov, int count);
ssize_t sock_sendmsg_file_iov(file *fp, const struct iovec *iov, int count);
int sock_to_fd(mos_sock *sk);
mos_sock *fd_to_sock(int fd);

/* ── lwIP callback helpers (sock_cb.c) ──────────────────────────────────── */

void sock_tcp_abort(mos_sock *sk);
void tcp_setup_callbacks(struct tcp_pcb *pcb, mos_sock *sk);
int raw_send_hdrincl(const void *buf, unsigned len);
err_t sock_tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *ip, u16_t port);
void sock_tcp_accept_listen(struct tcp_pcb *pcb, mos_sock *sk);
void sock_udp_recv_setup(struct udp_pcb *pcb, mos_sock *sk);
void sock_raw_recv_setup(struct raw_pcb *pcb, mos_sock *sk);

/* ── Socket operations (sock_ops.c) ─────────────────────────────────────── */

int do_socket(int domain, int type, int protocol);
int netlink_socket(int type, int protocol);
int netlink_bind(mos_sock *, const struct sockaddr *, unsigned);
int netlink_sockaddr(mos_sock *, struct sockaddr *, unsigned *);
int netlink_sendmsg(mos_sock *, const struct msghdr *, int);
int netlink_recvmsg(mos_sock *, struct msghdr *, int);
void netlink_release(mos_sock *);
int do_socketpair(int domain, int type, int protocol, int sv[2]);
int do_bind(int fd, const struct sockaddr *addr, unsigned addrlen);
int do_connect(int fd, const struct sockaddr *addr, unsigned addrlen);
int do_listen(int fd, int backlog);
int do_accept(int fd, struct sockaddr *addr, unsigned *addrlen);
int do_getsockname(int fd, struct sockaddr *addr, unsigned *addrlen);
int do_getpeername(int fd, struct sockaddr *addr, unsigned *addrlen);
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

size_t sock_msg_iov_total_len(const struct msghdr *msg);
int sock_msg_is_nonblock(int flags);
void sock_msg_cmsg_append(struct msghdr *msg, size_t *off, int level, int type,
			  const void *data, size_t dlen);
int do_sendmsg(int fd, const struct msghdr *msg, int flags);
int do_recvmsg(int fd, struct msghdr *msg, int flags);

/* ── AF_UNIX operations (sock_un.c) ─────────────────────────────────────── */

struct super_block;

int unix_sockaddr(mos_sock *sk, struct sockaddr *addr, unsigned *addrlen);
int unix_bind(mos_sock *sk, const struct sockaddr_un *addr, unsigned addrlen);
int unix_listen(mos_sock *sk, int backlog);
int unix_connect(mos_sock *sk, const struct sockaddr_un *addr,
		 unsigned addrlen);
int unix_accept(mos_sock *sk, struct sockaddr *addr, unsigned *addrlen,
		int nonblock);
/* Data-path operations require the caller's network core guard. */
ssize_t unix_read(file *fp, mos_sock *sk, void *buf, size_t count);
ssize_t unix_write(file *fp, mos_sock *sk, const void *buf, size_t count);
int unix_sendmsg(mos_sock *sk, const struct msghdr *msg, int flags);
int unix_recvmsg(mos_sock *sk, struct msghdr *msg, int flags);
void unix_drop_passfds(mos_sock *sk);
void unix_release(mos_sock *sk);
/* Remove a path from the Unix namespace; called by sys_unlink on socket files */
void unix_ns_remove_path(const char *path);

#endif /* _NET_SOCK_H */
