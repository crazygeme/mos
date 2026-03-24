/*
 * sock_ops.c — individual socket operations: socket/bind/connect/listen/
 *              accept/send/recv/shutdown.
 */
#include <net/sock.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <hw/time.h>
#include <ps/ps.h>
#include <fs/fs.h>
#include <errno.h>

#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/raw.h>
#include <lwip/ip4_addr.h>
#include <lwip/ip.h>

/* ── do_socket ───────────────────────────────────────────────────────────── */

int do_socket(int domain, int type, int protocol)
{
	if (domain != AF_INET)
		return -EAFNOSUPPORT;
	if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
		return -EPROTONOSUPPORT;
	if (type == SOCK_RAW && protocol != IPPROTO_ICMP &&
	    protocol != IPPROTO_RAW)
		return -EPROTONOSUPPORT;

	mos_sock *sk = zalloc(sizeof(*sk));
	if (!sk)
		return -ENOMEM;

	sk->domain = domain;
	sk->type = type;
	sk->protocol = protocol;
	sk->state = SS_UNCONNECTED;

	if (type == SOCK_STREAM) {
		sk->tcp = tcp_new();
		if (!sk->tcp) {
			free(sk);
			return -ENOMEM;
		}
		tcp_setup_callbacks(sk->tcp, sk);
	} else if (type == SOCK_RAW && protocol == IPPROTO_ICMP) {
		sk->raw = raw_new(IP_PROTO_ICMP);
		if (!sk->raw) {
			free(sk);
			return -ENOMEM;
		}
		sock_raw_recv_setup(sk->raw, sk);
	} else if (type == SOCK_RAW && protocol == IPPROTO_RAW) {
		sk->hdrincl = 1;
	} else {
		sk->udp = udp_new();
		if (!sk->udp) {
			free(sk);
			return -ENOMEM;
		}
		sock_udp_recv_setup(sk->udp, sk);
	}

	int fd = sock_to_fd(sk);
	if (fd < 0) {
		if (type == SOCK_STREAM)
			tcp_abort(sk->tcp);
		else if (type == SOCK_RAW && sk->raw)
			raw_remove(sk->raw);
		else if (sk->udp)
			udp_remove(sk->udp);
		free(sk);
		return -ENOMEM;
	}
	return fd;
}

/* ── do_socketpair ───────────────────────────────────────────────────────── */

int do_socketpair(int domain, int type, int protocol, int sv[2])
{
	if (domain != AF_UNIX)
		return -EAFNOSUPPORT;
	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return -EPROTONOSUPPORT;
	if (protocol != 0)
		return -EPROTONOSUPPORT;

	mos_sock *a = zalloc(sizeof(*a));
	mos_sock *b = zalloc(sizeof(*b));
	if (!a || !b) {
		free(a);
		free(b);
		return -ENOMEM;
	}

	a->domain = b->domain = domain;
	a->type = b->type = type;
	a->protocol = b->protocol = 0;
	a->state = b->state = SS_CONNECTED;
	a->unix_peer = b;
	b->unix_peer = a;
	spinlock_init(&a->rxbuf_lock);
	spinlock_init(&b->rxbuf_lock);

	int fd0 = sock_to_fd(a);
	if (fd0 < 0) {
		free(a);
		free(b);
		return -ENOMEM;
	}

	int fd1 = sock_to_fd(b);
	if (fd1 < 0) {
		fs_close(fd0); /* releases a; also clears b->unix_peer */
		free(b);
		return -ENOMEM;
	}

	sv[0] = fd0;
	sv[1] = fd1;
	return 0;
}

/* ── do_bind ─────────────────────────────────────────────────────────────── */

int do_bind(int fd, const struct sockaddr_in *addr, unsigned addrlen)
{
	(void)addrlen;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	ip_addr_t ip;
	ip4_addr_set_u32(ip_2_ip4(&ip), addr->sin_addr.s_addr);
	u16_t port = lwip_ntohs(addr->sin_port);

	err_t e;
	if (sk->type == SOCK_STREAM)
		e = tcp_bind(sk->tcp, &ip, port);
	else if (sk->type == SOCK_RAW)
		e = raw_bind(sk->raw, &ip);
	else
		e = udp_bind(sk->udp, &ip, port);

	if (e != ERR_OK)
		return (e == ERR_USE) ? -EADDRINUSE : -EINVAL;

	sk->local = *addr;
	return 0;
}

/* ── do_connect ──────────────────────────────────────────────────────────── */

int do_connect(int fd, const struct sockaddr_in *addr, unsigned addrlen)
{
	(void)addrlen;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	ip_addr_t ip;
	ip4_addr_set_u32(ip_2_ip4(&ip), addr->sin_addr.s_addr);
	u16_t port = lwip_ntohs(addr->sin_port);

	if (sk->type == SOCK_RAW) {
		err_t e = raw_connect(sk->raw, &ip);
		if (e != ERR_OK)
			return -EINVAL;
		sk->peer = *addr;
		sk->state = SS_CONNECTED;
		return 0;
	}

	if (sk->type == SOCK_DGRAM) {
		err_t e = udp_connect(sk->udp, &ip, port);
		if (e != ERR_OK)
			return -EINVAL;
		sk->peer = *addr;
		sk->state = SS_CONNECTED;
		return 0;
	}

	/* TCP — asynchronous connect */
	if (sk->state == SS_CONNECTED)
		return -EISCONN;

	sk->state = SS_CONNECTING;
	err_t e = sock_tcp_connect(sk->tcp, &ip, port);
	if (e != ERR_OK)
		return -ECONNREFUSED;

	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	while (sk->state == SS_CONNECTING) {
		if (sk->err)
			return sk->err;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sock_wait(sk, deadline);
	}
	if (sk->state != SS_CONNECTED)
		return sk->err ? sk->err : -ECONNREFUSED;

	sk->peer = *addr;
	return 0;
}

/* ── do_listen ───────────────────────────────────────────────────────────── */

int do_listen(int fd, int backlog)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->type != SOCK_STREAM)
		return -EOPNOTSUPP;

	u8_t bl = (u8_t)(backlog > SOCK_ACCEPT_BACKLOG ? SOCK_ACCEPT_BACKLOG :
							 backlog);
	struct tcp_pcb *lpcb = tcp_listen_with_backlog(sk->tcp, bl);
	if (!lpcb)
		return -ENOMEM;

	sk->tcp = lpcb;
	tcp_arg(sk->tcp, sk);
	sock_tcp_accept_listen(sk->tcp, sk);
	sk->state = SS_CONNECTED;
	return 0;
}

/* ── do_accept ───────────────────────────────────────────────────────────── */

int do_accept(int fd, struct sockaddr_in *addr, unsigned *addrlen)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->type != SOCK_STREAM)
		return -EOPNOTSUPP;

	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	while (sk->accept_head == sk->accept_tail) {
		if (sk->err)
			return sk->err;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sock_wait(sk, deadline);
	}

	struct tcp_pcb *newpcb = sk->accept_queue[sk->accept_head];
	sk->accept_head = (sk->accept_head + 1) % SOCK_ACCEPT_BACKLOG;
	tcp_backlog_accepted(sk->tcp);

	mos_sock *nsk = zalloc(sizeof(*nsk));
	if (!nsk) {
		tcp_abort(newpcb);
		return -ENOMEM;
	}
	nsk->domain = AF_INET;
	nsk->type = SOCK_STREAM;
	nsk->state = SS_CONNECTED;
	nsk->tcp = newpcb;
	tcp_setup_callbacks(newpcb, nsk);

	nsk->peer.sin_family = AF_INET;
	nsk->peer.sin_port = lwip_htons(newpcb->remote_port);
	nsk->peer.sin_addr.s_addr = ip4_addr_get_u32(&newpcb->remote_ip);

	if (addr && addrlen) {
		unsigned copy = *addrlen < sizeof(*addr) ? *addrlen :
							   sizeof(*addr);
		memcpy(addr, &nsk->peer, copy);
		*addrlen = sizeof(*addr);
	}

	int nfd = sock_to_fd(nsk);
	if (nfd < 0) {
		tcp_abort(newpcb);
		free(nsk);
		return -ENOMEM;
	}
	return nfd;
}

/* ── do_getsockname / do_getpeername ─────────────────────────────────────── */

int do_getsockname(int fd, struct sockaddr_in *addr, unsigned *addrlen)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	unsigned copy = *addrlen < sizeof(*addr) ? *addrlen : sizeof(*addr);
	memcpy(addr, &sk->local, copy);
	*addrlen = sizeof(*addr);
	return 0;
}

int do_getpeername(int fd, struct sockaddr_in *addr, unsigned *addrlen)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->state != SS_CONNECTED && sk->state != SS_DISCONNECTING)
		return -ENOTCONN;
	unsigned copy = *addrlen < sizeof(*addr) ? *addrlen : sizeof(*addr);
	memcpy(addr, &sk->peer, copy);
	*addrlen = sizeof(*addr);
	return 0;
}

/* ── do_send / do_recv ───────────────────────────────────────────────────── */

int do_send(int fd, const void *buf, unsigned len, int flags)
{
	(void)flags;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	loff_t pos = 0;
	task_struct *cur = CURRENT_TASK();
	return (int)cur->fds[fd].fp->f_fop->write(cur->fds[fd].fp, buf,
						  (size_t)len, &pos);
}

int do_recv(int fd, void *buf, unsigned len, int flags)
{
	return do_recvfrom(fd, buf, len, flags, NULL, NULL);
}

/* ── do_sendto ───────────────────────────────────────────────────────────── */

int do_sendto(int fd, const void *buf, unsigned len, int flags,
	      const struct sockaddr_in *to, unsigned tolen)
{
	(void)flags;
	(void)tolen;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->err)
		return sk->err;

	if (sk->type == SOCK_STREAM) {
		loff_t pos = 0;
		task_struct *cur = CURRENT_TASK();
		return (int)cur->fds[fd].fp->f_fop->write(cur->fds[fd].fp, buf,
							  len, &pos);
	}

	if (sk->type == SOCK_RAW) {
		if (sk->hdrincl)
			return raw_send_hdrincl(buf, len);
		ip_addr_t ip;
		ip4_addr_set_u32(ip_2_ip4(&ip), to->sin_addr.s_addr);
		struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)len, PBUF_RAM);
		if (!p)
			return -ENOBUFS;
		memcpy(p->payload, buf, len);
		err_t e = raw_sendto(sk->raw, p, &ip);
		pbuf_free(p);
		return e == ERR_OK ? (int)len : -EIO;
	}

	/* UDP */
	ip_addr_t ip;
	ip4_addr_set_u32(ip_2_ip4(&ip), to->sin_addr.s_addr);
	u16_t port = lwip_ntohs(to->sin_port);

	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
	if (!p)
		return -ENOBUFS;
	memcpy(p->payload, buf, len);
	err_t e = udp_sendto(sk->udp, p, &ip, port);
	pbuf_free(p);
	return e == ERR_OK ? (int)len : -EIO;
}

/* ── do_recvfrom ─────────────────────────────────────────────────────────── */

int do_recvfrom(int fd, void *buf, unsigned len, int flags,
		struct sockaddr_in *from, unsigned *fromlen)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;

	if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW) {
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->err)
				return sk->err;
			if (flags & MSG_DONTWAIT)
				return -EAGAIN;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			sock_wait(sk, deadline);
		}
		u16_t dlen;
		rx_read(sk, &dlen, sizeof(dlen));
		unsigned n = len < (unsigned)dlen ? len : (unsigned)dlen;
		rx_read(sk, buf, n);
		unsigned rem = (unsigned)dlen - n;
		while (rem--) {
			char tmp;
			rx_read(sk, &tmp, 1);
		}
		if (from && fromlen) {
			unsigned copy = *fromlen < sizeof(*from) ?
						*fromlen :
						sizeof(*from);
			memcpy(from, &sk->rx_src, copy);
			*fromlen = sizeof(*from);
		}
		return (int)n;
	}

	/* TCP */
	while (rx_used(sk) == 0) {
		if (sk->err)
			return sk->err;
		if (sk->state == SS_DISCONNECTING)
			return 0;
		if (sk->state == SS_UNCONNECTED)
			return -ENOTCONN;
		if (flags & MSG_DONTWAIT)
			return -EAGAIN;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sock_wait(sk, deadline);
	}
	unsigned n = rx_read(sk, buf, len);
	if (from && fromlen) {
		unsigned copy = *fromlen < sizeof(*from) ? *fromlen :
							   sizeof(*from);
		memcpy(from, &sk->peer, copy);
		*fromlen = sizeof(*from);
	}
	return (int)n;
}

/* ── do_shutdown ─────────────────────────────────────────────────────────── */

int do_shutdown(int fd, int how)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->type == SOCK_RAW) {
		raw_disconnect(sk->raw);
		sk->state = SS_UNCONNECTED;
		return 0;
	}
	if (sk->type != SOCK_STREAM)
		return -EOPNOTSUPP;
	if (!sk->tcp)
		return -ENOTCONN;
	int shut_rx = (how == SHUT_RD || how == SHUT_RDWR) ? 1 : 0;
	int shut_tx = (how == SHUT_WR || how == SHUT_RDWR) ? 1 : 0;
	tcp_shutdown(sk->tcp, shut_rx, shut_tx);
	return 0;
}
