/*
 * syscall_net.c — sys_socketcall for MOS.
 *
 * BSD socket API built on top of lwIP's NO_SYS raw TCP/UDP callbacks.
 * Blocking operations yield via task_sched() until lwIP's interrupt-driven
 * callbacks deliver data or a state change.
 *
 * Supported calls:
 *   SYS_SOCKET, SYS_BIND, SYS_CONNECT, SYS_LISTEN, SYS_ACCEPT,
 *   SYS_GETSOCKNAME, SYS_GETPEERNAME,
 *   SYS_SEND, SYS_RECV, SYS_SENDTO, SYS_RECVFROM,
 *   SYS_SHUTDOWN, SYS_SETSOCKOPT, SYS_GETSOCKOPT, SYS_ACCEPT4
 */
#include <net/socket.h>
#include <net/net.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <lib/timer.h>
#include <hw/time.h>
#include <ps/ps.h>
#include <errno.h>
#include <macro.h>

#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/netif.h>
#include <lwip/ip4_addr.h>
#include <lwip/pbuf.h>
#include <lwip/ip4_addr.h>
#include <lwip/timeouts.h>

/* ── Ring-buffer helpers ────────────────────────────────────────────────────── */

static inline unsigned rx_used(const mos_sock *sk)
{
	return (sk->rx_tail - sk->rx_head) & (SOCK_RXBUF_SIZE - 1);
}

static inline unsigned rx_free(const mos_sock *sk)
{
	return (SOCK_RXBUF_SIZE - 1) - rx_used(sk);
}

/* Write up to len bytes from src; returns bytes actually written. */
static unsigned rx_write(mos_sock *sk, const void *src, unsigned len)
{
	unsigned avail = rx_free(sk);
	unsigned n = len < avail ? len : avail;
	unsigned i;
	const char *s = (const char *)src;
	for (i = 0; i < n; i++) {
		sk->rxbuf[sk->rx_tail & (SOCK_RXBUF_SIZE - 1)] = s[i];
		sk->rx_tail++;
	}
	return n;
}

/* Read up to len bytes into dst; returns bytes actually read. */
static unsigned rx_read(mos_sock *sk, void *dst, unsigned len)
{
	unsigned avail = rx_used(sk);
	unsigned n = len < avail ? len : avail;
	unsigned i;
	char *d = (char *)dst;
	for (i = 0; i < n; i++) {
		d[i] = sk->rxbuf[sk->rx_head & (SOCK_RXBUF_SIZE - 1)];
		sk->rx_head++;
	}
	return n;
}

/* ── lwIP TCP callbacks ─────────────────────────────────────────────────────── */

static err_t tcp_on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
			 err_t err)
{
	mos_sock *sk = (mos_sock *)arg;
	(void)err;

	if (!p) {
		/* Remote closed the connection */
		sk->state = SS_DISCONNECTING;
		return ERR_OK;
	}

	/* Copy all pbuf data into our ring buffer */
	struct pbuf *q;
	u16_t acked = 0;
	for (q = p; q; q = q->next) {
		unsigned written = rx_write(sk, q->payload, (unsigned)q->len);
		acked += (u16_t)written;
		if (written < (unsigned)q->len)
			break; /* ring full — drop the rest */
	}
	tcp_recved(pcb, acked);
	pbuf_free(p);
	return ERR_OK;
}

static err_t tcp_on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
	mos_sock *sk = (mos_sock *)arg;
	(void)pcb;
	if (err != ERR_OK) {
		sk->err = -ECONNREFUSED;
		sk->state = SS_UNCONNECTED;
	} else {
		sk->state = SS_CONNECTED;
	}
	return ERR_OK;
}

static void tcp_on_err(void *arg, err_t err)
{
	mos_sock *sk = (mos_sock *)arg;
	(void)err;
	/* PCB has already been freed by lwIP — null it out */
	sk->tcp = NULL;
	sk->err = -ECONNREFUSED;
	sk->state = SS_UNCONNECTED;
}

static err_t tcp_on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	mos_sock *sk = (mos_sock *)arg;
	if (err != ERR_OK)
		return err;

	int next = (sk->accept_tail + 1) % SOCK_ACCEPT_BACKLOG;
	if (next == sk->accept_head)
		return ERR_MEM; /* queue full */

	sk->accept_queue[sk->accept_tail] = newpcb;
	sk->accept_tail = next;
	return ERR_OK;
}

/* ── lwIP UDP callback ──────────────────────────────────────────────────────── */

static void udp_on_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
			const ip_addr_t *addr, u16_t port)
{
	mos_sock *sk = (mos_sock *)arg;
	(void)pcb;

	if (!p)
		return;

	/* Store source for recvfrom */
	sk->rx_src.sin_family = AF_INET;
	sk->rx_src.sin_port = lwip_htons(port);
	sk->rx_src.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(addr));

	/* Prefix the datagram in the ring with a 2-byte little-endian length */
	u16_t dlen = p->tot_len;
	if (rx_free(sk) >= (unsigned)(sizeof(dlen) + dlen)) {
		rx_write(sk, &dlen, sizeof(dlen));
		struct pbuf *q;
		for (q = p; q; q = q->next)
			rx_write(sk, q->payload, (unsigned)q->len);
	}
	pbuf_free(p);
}

/* ── Socket file operations ─────────────────────────────────────────────────── */

static ssize_t sock_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	(void)pos;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	if (sk->type == SOCK_DGRAM) {
		/* UDP: block until a datagram arrives */
		unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->err)
				return sk->err;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			sys_check_timeouts();
			task_sched();
		}
		u16_t dlen;
		rx_read(sk, &dlen, sizeof(dlen));
		unsigned n = (unsigned)count < (unsigned)dlen ?
				     (unsigned)count :
				     (unsigned)dlen;
		rx_read(sk, buf, n);
		/* discard any remainder */
		if (n < (unsigned)dlen) {
			char tmp;
			unsigned rem = (unsigned)dlen - n;
			while (rem--)
				rx_read(sk, &tmp, 1);
		}
		return (ssize_t)n;
	}

	/* TCP: block until data or EOF */
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	while (rx_used(sk) == 0) {
		if (sk->err)
			return sk->err;
		if (sk->state == SS_DISCONNECTING)
			return 0; /* EOF */
		if (sk->state == SS_UNCONNECTED)
			return -ENOTCONN;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sys_check_timeouts();
		task_sched();
	}
	unsigned n = rx_read(sk, buf, (unsigned)count);
	return (ssize_t)n;
}

static ssize_t sock_write(file *fp, const void *buf, size_t count, loff_t *pos)
{
	(void)pos;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	if (sk->err)
		return sk->err;

	if (sk->type == SOCK_DGRAM) {
		if (!sk->udp)
			return -ENOTCONN;
		struct pbuf *p =
			pbuf_alloc(PBUF_TRANSPORT, (u16_t)count, PBUF_RAM);
		if (!p)
			return -ENOBUFS;
		memcpy(p->payload, buf, count);
		err_t e = udp_send(sk->udp, p);
		pbuf_free(p);
		return e == ERR_OK ? (ssize_t)count : -EIO;
	}

	/* TCP */
	if (sk->state != SS_CONNECTED && sk->state != SS_DISCONNECTING)
		return -ENOTCONN;

	err_t e = tcp_write(sk->tcp, buf, (u16_t)count, TCP_WRITE_FLAG_COPY);
	if (e != ERR_OK)
		return (e == ERR_MEM) ? -ENOBUFS : -EIO;
	tcp_output(sk->tcp);
	return (ssize_t)count;
}

static int sock_release(file *fp)
{
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	if (sk->type == SOCK_DGRAM) {
		if (sk->udp)
			udp_remove(sk->udp);
	} else {
		if (sk->tcp) {
			tcp_arg(sk->tcp, NULL);
			tcp_recv(sk->tcp, NULL);
			tcp_err(sk->tcp, NULL);
			tcp_close(sk->tcp);
		}
	}

	free(sk);
	free(fp->f_inode);
	free(fp);
	return 0;
}

/* ── helpers for filling ifreq fields ───────────────────────────────────────── */

/* Copy a 4-byte IPv4 address into ifr_addr / ifr_netmask / ifr_broadaddr. */
static void fill_ifr_sin(struct ifreq *ifr, uint32_t addr_nbo)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr_nbo;
}

/* Fill a single ifreq for the given lwIP netif (index 1-based). */
static void fill_eth0_ifreq(struct ifreq *ifr, struct netif *nif, unsigned cmd)
{
	/* Interface name: e.g. "e00" → match lwIP name[2]+num */
	sprintf(ifr->ifr_name, "%c%c%u", nif->name[0], nif->name[1],
		(unsigned)nif->num);

	switch (cmd) {
	case SIOCGIFFLAGS: {
		short f = IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST;
		ifr->ifr_flags = f;
		break;
	}
	case SIOCGIFADDR:
	case SIOCGIFCONF: /* SIOCGIFCONF fills addr for each entry */
		fill_ifr_sin(ifr, ip4_addr_get_u32(netif_ip4_addr(nif)));
		break;
	case SIOCGIFNETMASK:
		fill_ifr_sin(ifr, ip4_addr_get_u32(netif_ip4_netmask(nif)));
		break;
	case SIOCGIFBRDADDR: {
		/* broadcast = ip | ~mask */
		uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(nif));
		uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(nif));
		fill_ifr_sin(ifr, ip | ~mask);
		break;
	}
	case SIOCGIFHWADDR: {
		struct sockaddr *sa = &ifr->ifr_hwaddr;
		memset(sa, 0, sizeof(*sa));
		sa->sa_family = ARPHRD_ETHER;
		memcpy(sa->sa_data, nif->hwaddr, 6);
		break;
	}
	case SIOCGIFMTU:
		ifr->ifr_mtu = nif->mtu;
		break;
	case SIOCGIFINDEX:
		ifr->ifr_ifindex = 1;
		break;
	case SIOCGIFMETRIC:
		ifr->ifr_metric = 0;
		break;
	default:
		break;
	}
}

/* Fill a single ifreq for the loopback interface. */
static void fill_lo_ifreq(struct ifreq *ifr, unsigned cmd)
{
	strncpy(ifr->ifr_name, "lo", IFNAMSIZ);

	switch (cmd) {
	case SIOCGIFFLAGS:
		ifr->ifr_flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
		break;
	case SIOCGIFADDR:
	case SIOCGIFCONF: {
		/* 127.0.0.1 */
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = lwip_htonl(0x7F000001UL);
		break;
	}
	case SIOCGIFNETMASK: {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = lwip_htonl(0xFF000000UL);
		break;
	}
	case SIOCGIFBRDADDR:
		fill_ifr_sin(ifr, 0);
		break;
	case SIOCGIFHWADDR:
		memset(&ifr->ifr_hwaddr, 0, sizeof(struct sockaddr));
		ifr->ifr_hwaddr.sa_family = ARPHRD_ETHER;
		break;
	case SIOCGIFMTU:
		ifr->ifr_mtu = 65536;
		break;
	case SIOCGIFINDEX:
		ifr->ifr_ifindex = 2;
		break;
	case SIOCGIFMETRIC:
		ifr->ifr_metric = 0;
		break;
	default:
		break;
	}
}

/* ── sock_ioctl ─────────────────────────────────────────────────────────────── */

static int sock_ioctl(file *fp, unsigned cmd, void *arg)
{
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	switch (cmd) {
	case FIONREAD: {
		*(int *)arg = (int)rx_used(sk);
		return 0;
	}

	case FIONBIO:
		/* Non-blocking mode not implemented; silently accept. */
		return 0;

	case SIOCGIFCONF: {
		struct ifconf *ifc = (struct ifconf *)arg;
		struct ifreq *req = ifc->ifc_req;
		int max = ifc->ifc_len / (int)sizeof(struct ifreq);
		int n = 0;

		struct netif *nif = net_get_default_netif();

		/* eth0 */
		if (n < max && nif) {
			memset(&req[n], 0, sizeof(struct ifreq));
			fill_eth0_ifreq(&req[n], nif, SIOCGIFCONF);
			n++;
		}
		/* lo */
		if (n < max) {
			memset(&req[n], 0, sizeof(struct ifreq));
			fill_lo_ifreq(&req[n], SIOCGIFCONF);
			n++;
		}

		ifc->ifc_len = n * (int)sizeof(struct ifreq);
		return 0;
	}

	case SIOCGIFFLAGS:
	case SIOCGIFADDR:
	case SIOCGIFNETMASK:
	case SIOCGIFBRDADDR:
	case SIOCGIFHWADDR:
	case SIOCGIFMTU:
	case SIOCGIFINDEX:
	case SIOCGIFMETRIC: {
		struct ifreq *ifr = (struct ifreq *)arg;
		struct netif *nif = net_get_default_netif();

		/* Match by interface name */
		if (nif) {
			char eth_name[IFNAMSIZ];
			sprintf(eth_name, "%c%c%u", nif->name[0], nif->name[1],
				(unsigned)nif->num);
			if (strncmp(ifr->ifr_name, eth_name, IFNAMSIZ) == 0) {
				fill_eth0_ifreq(ifr, nif, cmd);
				return 0;
			}
		}
		if (strncmp(ifr->ifr_name, "lo", IFNAMSIZ) == 0) {
			fill_lo_ifreq(ifr, cmd);
			return 0;
		}
		return -ENODEV;
	}

	default:
		return -ENOTTY;
	}
}

static const file_operations sock_fops = {
	.read = sock_read,
	.write = sock_write,
	.ioctl = sock_ioctl,
	.release = sock_release,
};

/* ── Helper: wrap a mos_sock in a file and install as an fd ─────────────────── */

static int sock_to_fd(mos_sock *sk)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFSOCK | 0600;
	node->i_private = sk;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &sock_fops;
	fp->f_mode = O_RDWR;

	int fd = fs_install_fd(fp, O_RDWR);
	if (fd < 0) {
		free(node);
		free(fp);
	}
	return fd;
}

/* Retrieve mos_sock from an fd, or NULL. */
static mos_sock *fd_to_sock(int fd)
{
	task_struct *cur = CURRENT_TASK();
	if (fd < 0 || fd >= (int)MAX_FD)
		return NULL;
	if (!cur->fds[fd].used)
		return NULL;
	file *fp = cur->fds[fd].fp;
	if (!fp || !fp->f_inode || !S_ISSOCK(fp->f_inode->i_mode))
		return NULL;
	return (mos_sock *)fp->f_inode->i_private;
}

/* ── Install callbacks on a new TCP pcb ─────────────────────────────────────── */

static void tcp_setup_callbacks(struct tcp_pcb *pcb, mos_sock *sk)
{
	tcp_arg(pcb, sk);
	tcp_recv(pcb, tcp_on_recv);
	tcp_err(pcb, tcp_on_err);
}

/* ── Individual socket operations ───────────────────────────────────────────── */

static int do_socket(int domain, int type, int protocol)
{
	if (domain != AF_INET)
		return -EAFNOSUPPORT;
	if (type != SOCK_STREAM && type != SOCK_DGRAM)
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
	} else {
		sk->udp = udp_new();
		if (!sk->udp) {
			free(sk);
			return -ENOMEM;
		}
		udp_recv(sk->udp, udp_on_recv, sk);
	}

	int fd = sock_to_fd(sk);
	if (fd < 0) {
		if (type == SOCK_STREAM)
			tcp_abort(sk->tcp);
		else
			udp_remove(sk->udp);
		free(sk);
		return -ENOMEM;
	}
	return fd;
}

static int do_bind(int fd, const struct sockaddr_in *addr, unsigned addrlen)
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
	else
		e = udp_bind(sk->udp, &ip, port);

	if (e != ERR_OK)
		return (e == ERR_USE) ? -EADDRINUSE : -EINVAL;

	sk->local = *addr;
	return 0;
}

static int do_connect(int fd, const struct sockaddr_in *addr, unsigned addrlen)
{
	(void)addrlen;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	ip_addr_t ip;
	ip4_addr_set_u32(ip_2_ip4(&ip), addr->sin_addr.s_addr);
	u16_t port = lwip_ntohs(addr->sin_port);

	if (sk->type == SOCK_DGRAM) {
		err_t e = udp_connect(sk->udp, &ip, port);
		if (e != ERR_OK)
			return -EINVAL;
		sk->peer = *addr;
		sk->state = SS_CONNECTED;
		return 0;
	}

	/* TCP — asynchronous connect, then spin-wait */
	if (sk->state == SS_CONNECTED)
		return -EISCONN;

	sk->state = SS_CONNECTING;
	err_t e = tcp_connect(sk->tcp, &ip, port, tcp_on_connected);
	if (e != ERR_OK)
		return -ECONNREFUSED;

	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	while (sk->state == SS_CONNECTING) {
		if (sk->err)
			return sk->err;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sys_check_timeouts();
		task_sched();
	}
	if (sk->state != SS_CONNECTED)
		return sk->err ? sk->err : -ECONNREFUSED;

	sk->peer = *addr;
	return 0;
}

static int do_listen(int fd, int backlog)
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
	tcp_accept(sk->tcp, tcp_on_accept);
	sk->state = SS_CONNECTED; /* listening */
	return 0;
}

static int do_accept(int fd, struct sockaddr_in *addr, unsigned *addrlen)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->type != SOCK_STREAM)
		return -EOPNOTSUPP;

	/* Block until the accept queue has an entry */
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
	while (sk->accept_head == sk->accept_tail) {
		if (sk->err)
			return sk->err;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sys_check_timeouts();
		task_sched();
	}

	struct tcp_pcb *newpcb = sk->accept_queue[sk->accept_head];
	sk->accept_head = (sk->accept_head + 1) % SOCK_ACCEPT_BACKLOG;
	tcp_backlog_accepted(sk->tcp);

	/* Build a new mos_sock for the accepted connection */
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

	/* Fill peer address */
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

static int do_getsockname(int fd, struct sockaddr_in *addr, unsigned *addrlen)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	unsigned copy = *addrlen < sizeof(*addr) ? *addrlen : sizeof(*addr);
	memcpy(addr, &sk->local, copy);
	*addrlen = sizeof(*addr);
	return 0;
}

static int do_getpeername(int fd, struct sockaddr_in *addr, unsigned *addrlen)
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

static int do_send(int fd, const void *buf, unsigned len, int flags)
{
	(void)flags;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	loff_t pos = 0;
	task_struct *cur = CURRENT_TASK();
	return (int)sock_write(cur->fds[fd].fp, buf, (size_t)len, &pos);
}

static int do_recv(int fd, void *buf, unsigned len, int flags)
{
	(void)flags;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	loff_t pos = 0;
	task_struct *cur = CURRENT_TASK();
	return (int)sock_read(cur->fds[fd].fp, buf, (size_t)len, &pos);
}

static int do_sendto(int fd, const void *buf, unsigned len, int flags,
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
		/* For TCP, sendto acts like send (peer addr from connect) */
		loff_t pos = 0;
		task_struct *cur = CURRENT_TASK();
		return (int)sock_write(cur->fds[fd].fp, buf, len, &pos);
	}

	/* UDP with explicit destination */
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

static int do_recvfrom(int fd, void *buf, unsigned len, int flags,
		       struct sockaddr_in *from, unsigned *fromlen)
{
	(void)flags;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	/* Block until data */
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;

	if (sk->type == SOCK_DGRAM) {
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->err)
				return sk->err;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			sys_check_timeouts();
			task_sched();
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
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sys_check_timeouts();
		task_sched();
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

static int do_shutdown(int fd, int how)
{
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	if (sk->type != SOCK_STREAM)
		return -EOPNOTSUPP;
	if (!sk->tcp)
		return -ENOTCONN;
	int shut_rx = (how == SHUT_RD || how == SHUT_RDWR) ? 1 : 0;
	int shut_tx = (how == SHUT_WR || how == SHUT_RDWR) ? 1 : 0;
	tcp_shutdown(sk->tcp, shut_rx, shut_tx);
	return 0;
}

static int do_setsockopt(int fd, int level, int optname, const void *optval,
			 unsigned optlen)
{
	(void)level;
	(void)optname;
	(void)optval;
	(void)optlen;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;
	/* Accept but ignore all socket options for now */
	return 0;
}

static int do_getsockopt(int fd, int level, int optname, void *optval,
			 unsigned *optlen)
{
	(void)level;
	(void)optname;
	mos_sock *sk = fd_to_sock(fd);
	if (!sk)
		return -ENOTSOCK;

	/* Return 0 (no error) for SO_ERROR; reject everything else */
	if (optval && optlen && *optlen >= sizeof(int)) {
		*(int *)optval = 0;
		*optlen = sizeof(int);
	}
	return 0;
}

/* ── sys_socketcall dispatcher ──────────────────────────────────────────────── */

int sys_socketcall(int call, unsigned long *args)
{
	switch (call) {
	case SYS_SOCKET:
		return do_socket((int)args[0], (int)args[1], (int)args[2]);

	case SYS_BIND:
		return do_bind((int)args[0],
			       (const struct sockaddr_in *)args[1],
			       (unsigned)args[2]);

	case SYS_CONNECT:
		return do_connect((int)args[0],
				  (const struct sockaddr_in *)args[1],
				  (unsigned)args[2]);

	case SYS_LISTEN:
		return do_listen((int)args[0], (int)args[1]);

	case SYS_ACCEPT:
	case SYS_ACCEPT4:
		return do_accept((int)args[0], (struct sockaddr_in *)args[1],
				 (unsigned *)args[2]);

	case SYS_GETSOCKNAME:
		return do_getsockname((int)args[0],
				      (struct sockaddr_in *)args[1],
				      (unsigned *)args[2]);

	case SYS_GETPEERNAME:
		return do_getpeername((int)args[0],
				      (struct sockaddr_in *)args[1],
				      (unsigned *)args[2]);

	case SYS_SEND:
		return do_send((int)args[0], (const void *)args[1],
			       (unsigned)args[2], (int)args[3]);

	case SYS_RECV:
		return do_recv((int)args[0], (void *)args[1], (unsigned)args[2],
			       (int)args[3]);

	case SYS_SENDTO:
		return do_sendto((int)args[0], (const void *)args[1],
				 (unsigned)args[2], (int)args[3],
				 (const struct sockaddr_in *)args[4],
				 (unsigned)args[5]);

	case SYS_RECVFROM:
		return do_recvfrom((int)args[0], (void *)args[1],
				   (unsigned)args[2], (int)args[3],
				   (struct sockaddr_in *)args[4],
				   (unsigned *)args[5]);

	case SYS_SHUTDOWN:
		return do_shutdown((int)args[0], (int)args[1]);

	case SYS_SETSOCKOPT:
		return do_setsockopt((int)args[0], (int)args[1], (int)args[2],
				     (const void *)args[3], (unsigned)args[4]);

	case SYS_GETSOCKOPT:
		return do_getsockopt((int)args[0], (int)args[1], (int)args[2],
				     (void *)args[3], (unsigned *)args[4]);

	default:
		return -ENOSYS;
	}
}
