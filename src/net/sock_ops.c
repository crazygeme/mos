/*
 * sock_ops.c — individual socket operations: socket/bind/connect/listen/
 *              accept/send/recv/shutdown.
 */
#include <net/sock.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <ps/ps.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <errno.h>

#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/raw.h>
#include <lwip/ip4_addr.h>
#include <lwip/ip.h>

static void sock_refresh_local_inet(mos_sock *sk)
{
	sk->local.sin_family = AF_INET;

	if (sk->type == SOCK_STREAM && sk->tcp) {
		sk->local.sin_port = lwip_htons(sk->tcp->local_port);
		sk->local.sin_addr.s_addr =
			ip4_addr_get_u32(&sk->tcp->local_ip);
		return;
	}

	if (sk->type == SOCK_DGRAM && sk->udp) {
		sk->local.sin_port = lwip_htons(sk->udp->local_port);
		sk->local.sin_addr.s_addr =
			ip4_addr_get_u32(ip_2_ip4(&sk->udp->local_ip));
		return;
	}

	if (sk->type == SOCK_RAW && sk->raw) {
		sk->local.sin_port = 0;
		sk->local.sin_addr.s_addr =
			ip4_addr_get_u32(ip_2_ip4(&sk->raw->local_ip));
	}
}

/* ── do_socket ───────────────────────────────────────────────────────────── */

int do_socket(int domain, int type, int protocol)
{
	if (TestControl.verbos)
		klog("socket(domain=%d, type=%d, protocol=%d)\n", domain, type,
		     protocol);

	if (domain == AF_UNIX) {
		if (type != SOCK_STREAM && type != SOCK_DGRAM)
			return -EPROTONOSUPPORT;
		if (protocol != 0)
			return -EPROTONOSUPPORT;
		mos_sock *sk = zalloc(sizeof(*sk));
		if (!sk)
			return -ENOMEM;
		sk->domain = AF_UNIX;
		sk->type = type;
		sk->state = SS_UNCONNECTED;
		spinlock_init(&sk->wait_lock);
		list_init(&sk->waiters);
		list_init(&sk->poll_waiters);
		spinlock_init(&sk->rxbuf_lock);
		int fd = sock_to_fd(sk);
		if (fd < 0) {
			free(sk);
			return -ENOMEM;
		}
		return fd;
	}

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
	spinlock_init(&sk->wait_lock);
	list_init(&sk->waiters);
	list_init(&sk->poll_waiters);

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

/* ── do_bind ─────────────────────────────────────────────────────────────── */

int do_bind(int fd, const struct sockaddr *addr, unsigned addrlen)
{
	if (TestControl.verbos)
		klog("bind(fd=%d, addr=%x, addrlen=%u)\n", fd, addr, addrlen);

	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	if (addr->sa_family == AF_UNIX)
		return unix_bind(sk, (const struct sockaddr_un *)addr, addrlen);

	/* AF_INET */
	const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
	ip_addr_t ip;
	ip4_addr_set_u32(ip_2_ip4(&ip), in->sin_addr.s_addr);
	u16_t port = lwip_ntohs(in->sin_port);

	err_t e;
	if (sk->type == SOCK_STREAM) {
		if (sk->tcp->local_port != 0)
			return -EINVAL;
		e = tcp_bind(sk->tcp, &ip, port);
	} else if (sk->type == SOCK_RAW) {
		e = raw_bind(sk->raw, &ip);
	} else {
		if (sk->udp->local_port != 0)
			return -EINVAL;
		e = udp_bind(sk->udp, &ip, port);
	}

	if (e != ERR_OK)
		return (e == ERR_USE) ? -EADDRINUSE : -EINVAL;

	sk->local = *in;
	sock_refresh_local_inet(sk);
	return 0;
}

/* ── do_connect ──────────────────────────────────────────────────────────── */

int do_connect(int fd, const struct sockaddr *addr, unsigned addrlen)
{
	if (TestControl.verbos)
		klog("connect(fd=%d, addr=%x, addrlen=%u)\n", fd, addr,
		     addrlen);

	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	if (addr->sa_family == AF_UNIX)
		return unix_connect(sk, (const struct sockaddr_un *)addr,
				    addrlen);

	const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
	ip_addr_t ip;
	ip4_addr_set_u32(ip_2_ip4(&ip), in->sin_addr.s_addr);
	u16_t port = lwip_ntohs(in->sin_port);

	if (sk->type == SOCK_RAW) {
		err_t e = raw_connect(sk->raw, &ip);
		if (e != ERR_OK)
			return -EINVAL;
		sk->peer = *in;
		sk->state = SS_CONNECTED;
		return 0;
	}

	if (sk->type == SOCK_DGRAM) {
		err_t e = udp_connect(sk->udp, &ip, port);
		if (e != ERR_OK)
			return -EINVAL;
		sk->peer = *in;
		sock_refresh_local_inet(sk);
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
		if (sock_wait(sk, deadline) < 0)
			return -EINTR;
	}
	if (sk->state != SS_CONNECTED)
		return sk->err ? sk->err : -ECONNREFUSED;

	sk->peer = *in;
	sock_refresh_local_inet(sk);
	return 0;
}

/* ── do_listen ───────────────────────────────────────────────────────────── */

int do_listen(int fd, int backlog)
{
	if (TestControl.verbos)
		klog("listen(fd=%d, backlog=%d)\n", fd, backlog);

	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->domain == AF_UNIX)
		return unix_listen(sk, backlog);
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

int do_accept(int fd, struct sockaddr *addr, unsigned *addrlen)
{
	if (TestControl.verbos)
		klog("accept(fd=%d, addr=%x, addrlen=%x)\n", fd, addr, addrlen);

	mos_sock *sk = fd_to_sock(fd);
	task_struct *cur = CURRENT_TASK();
	int nonblock;
	if (!sk)
		return -ENOTSOCK;
	nonblock = cur->fds[fd] && (cur->fds[fd]->f_flag & O_NONBLOCK) != 0;
	if (sk->domain == AF_UNIX)
		return unix_accept(sk, addr, addrlen, nonblock);
	if (sk->type != SOCK_STREAM)
		return -EOPNOTSUPP;

	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	while (sk->accept_head == sk->accept_tail) {
		if (sk->err)
			return sk->err;
		if (nonblock)
			return -EAGAIN;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		if (sock_wait(sk, deadline) < 0)
			return -EINTR;
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
	spinlock_init(&nsk->wait_lock);
	list_init(&nsk->waiters);
	list_init(&nsk->poll_waiters);
	tcp_setup_callbacks(newpcb, nsk);

	nsk->peer.sin_family = AF_INET;
	nsk->peer.sin_port = lwip_htons(newpcb->remote_port);
	nsk->peer.sin_addr.s_addr = ip4_addr_get_u32(&newpcb->remote_ip);
	nsk->local.sin_family = AF_INET;
	nsk->local.sin_port = lwip_htons(newpcb->local_port);
	nsk->local.sin_addr.s_addr = ip4_addr_get_u32(&newpcb->local_ip);

	if (addr && addrlen) {
		unsigned copy = *addrlen < sizeof(nsk->peer) ?
					*addrlen :
					sizeof(nsk->peer);
		memcpy(addr, &nsk->peer, copy);
		*addrlen = sizeof(nsk->peer);
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

int do_getsockname(int fd, struct sockaddr *addr, unsigned *addrlen)
{
	if (TestControl.verbos)
		klog("getsockname(fd=%d, addr=%x, addrlen=%x)\n", fd, addr,
		     addrlen);

	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	if (sk->domain == AF_UNIX) {
		struct sockaddr_un *un = (struct sockaddr_un *)addr;
		unsigned copy = *addrlen < sizeof(*un) ? *addrlen : sizeof(*un);
		un->sun_family = AF_UNIX;
		strncpy(un->sun_path, sk->unix_path, UNIX_PATH_MAX - 1);
		memcpy(addr, un, copy);
		*addrlen = sizeof(*un);
		return 0;
	}

	sock_refresh_local_inet(sk);
	unsigned copy = *addrlen < sizeof(sk->local) ? *addrlen :
						       sizeof(sk->local);
	memcpy(addr, &sk->local, copy);
	*addrlen = sizeof(sk->local);
	return 0;
}

int do_getpeername(int fd, struct sockaddr *addr, unsigned *addrlen)
{
	if (TestControl.verbos)
		klog("getpeername(fd=%d, addr=%x, addrlen=%x)\n", fd, addr,
		     addrlen);

	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->state != SS_CONNECTED && sk->state != SS_DISCONNECTING)
		return -ENOTCONN;

	if (sk->domain == AF_UNIX) {
		struct sockaddr_un *un = (struct sockaddr_un *)addr;
		unsigned copy = *addrlen < sizeof(*un) ? *addrlen : sizeof(*un);
		un->sun_family = AF_UNIX;
		strncpy(un->sun_path,
			sk->unix_peer ? sk->unix_peer->unix_path : "",
			UNIX_PATH_MAX - 1);
		memcpy(addr, un, copy);
		*addrlen = sizeof(*un);
		return 0;
	}

	unsigned copy = *addrlen < sizeof(sk->peer) ? *addrlen :
						      sizeof(sk->peer);
	memcpy(addr, &sk->peer, copy);
	*addrlen = sizeof(sk->peer);
	return 0;
}

/* ── do_send / do_recv ───────────────────────────────────────────────────── */

int do_send(int fd, const void *buf, unsigned len, int flags)
{
	if (TestControl.verbos)
		klog("send(fd=%d, buf=%x, len=%u, flags=%d)\n", fd, buf, len,
		     flags);

	(void)flags;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	loff_t pos = 0;
	task_struct *cur = CURRENT_TASK();
	return (int)cur->fds[fd]->f_fop->write(cur->fds[fd], buf, (size_t)len,
					       &pos);
}

int do_recv(int fd, void *buf, unsigned len, int flags)
{
	if (TestControl.verbos)
		klog("recv(fd=%d, buf=%x, len=%u, flags=%d)\n", fd, buf, len,
		     flags);

	return do_recvfrom(fd, buf, len, flags, NULL, NULL);
}

/* ── do_sendto ───────────────────────────────────────────────────────────── */

int do_sendto(int fd, const void *buf, unsigned len, int flags,
	      const struct sockaddr_in *to, unsigned tolen)
{
	if (TestControl.verbos)
		klog("sendto(fd=%d, buf=%x, len=%u, flags=%d, to=%x, tolen=%u)\n",
		     fd, buf, len, flags, to, tolen);

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
		return (int)cur->fds[fd]->f_fop->write(cur->fds[fd], buf, len,
						       &pos);
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
	if (TestControl.verbos)
		klog("recvfrom(fd=%d, buf=%x, len=%u, flags=%d, from=%x, fromlen=%x)\n",
		     fd, buf, len, flags, from, fromlen);

	mos_sock *sk = fd_to_sock(fd);
	task_struct *cur = CURRENT_TASK();
	if (!sk)
		return -ENOTSOCK;
	if (cur->fds[fd] && (cur->fds[fd]->f_flag & O_NONBLOCK))
		flags |= MSG_DONTWAIT;

	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;

	if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW) {
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->err)
				return sk->err;
			if (flags & MSG_DONTWAIT)
				return -EAGAIN;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			if (sock_wait(sk, deadline) < 0)
				return -EINTR;
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
		if (sock_wait(sk, deadline) < 0)
			return -EINTR;
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
	if (TestControl.verbos)
		klog("shutdown(fd=%d, how=%d)\n", fd, how);

	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	if (sk->domain == AF_UNIX) {
		(void)how;
		mos_sock *peer = sk->unix_peer;
		if (peer && sk->type == SOCK_STREAM) {
			peer->unix_peer = NULL;
			peer->state = SS_DISCONNECTING;
			sock_wakeup(peer);
		}
		sk->state = SS_DISCONNECTING;
		return 0;
	}

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
