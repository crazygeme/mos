/*
 * sock.c — MOS socket core: ring buffer, blocking helpers,
 *          file operations, ioctl, and fd/sock helpers.
 */
#include <net/sock.h>
#include <net/net.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/timer.h>
#include <hw/time.h>
#include <ps/ps.h>
#include <errno.h>
#include <macro.h>

#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/raw.h>
#include <lwip/netif.h>
#include <lwip/ip4_addr.h>
#include <lwip/pbuf.h>

/* ── Ring-buffer helpers ─────────────────────────────────────────────────── */

unsigned rx_used(const mos_sock *sk)
{
	return (sk->rx_tail - sk->rx_head) & (SOCK_RXBUF_SIZE - 1);
}

unsigned rx_free(const mos_sock *sk)
{
	return (SOCK_RXBUF_SIZE - 1) - rx_used(sk);
}

/* Write up to len bytes from src; returns bytes actually written. */
unsigned rx_write(mos_sock *sk, const void *src, unsigned len)
{
	unsigned avail = rx_free(sk);
	unsigned n = len < avail ? len : avail;
	unsigned i;
	const char *s = (const char *)src;
	unsigned long long now = time_now_us();
	for (i = 0; i < n; i++) {
		sk->rxbuf[sk->rx_tail & (SOCK_RXBUF_SIZE - 1)] = s[i];
		sk->rx_tail++;
	}
	if (n > 0)
		us_to_timeval(now, &sk->rx_stamp);

	return n;
}

/* Read up to len bytes into dst; returns bytes actually read. */
unsigned rx_read(mos_sock *sk, void *dst, unsigned len)
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

/* ── Blocking helpers ────────────────────────────────────────────────────── */

/* Wake the task currently blocked on sk, if any.
 * Safe to call from lwIP callbacks (NIC IRQ context) and timer callbacks. */
void sock_wakeup(mos_sock *sk)
{
	task_struct *t = sk->waiter;
	if (t) {
		sk->waiter = NULL;
		ps_put_to_ready_queue(t);
	}
}

/* One-shot timer callback: fires at the deadline and unblocks the waiter. */
static void sock_timeout_cb(timer_t *t, void *ctx)
{
	(void)t;
	sock_wakeup((mos_sock *)ctx);
}

/* Block the current task on sk until woken by sock_wakeup or the deadline. */
void sock_wait(mos_sock *sk, unsigned long deadline)
{
	unsigned long now = time_now_ms();
	if (now >= deadline)
		return;

	task_struct *cur = CURRENT_TASK();
	timer_start(sock_timeout_cb, (unsigned)(deadline - now), 0, sk);
	sk->waiter = cur;
	ps_put_to_wait_queue(cur, NULL, __func__);
	task_sched();
	sk->waiter = NULL;
}

/* ── Socket file operations ──────────────────────────────────────────────── */

/*
 * Read path for AF_UNIX sockets.  The spinlock serialises concurrent readers
 * (e.g. parent and child after fork both draining the same rxbuf).  The lock
 * must be released before calling sock_wait() because sock_wait() sleeps.
 */
static ssize_t sock_unix_read(mos_sock *sk, void *buf, size_t count)
{
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;

	if (sk->type == SOCK_DGRAM) {
		spinlock_lock(&sk->rxbuf_lock);
		while (rx_used(sk) < sizeof(u16_t)) {
			spinlock_unlock(&sk->rxbuf_lock);
			if (sk->err)
				return sk->err;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			sock_wait(sk, deadline);
			spinlock_lock(&sk->rxbuf_lock);
		}
		u16_t dlen;
		rx_read(sk, &dlen, sizeof(dlen));
		unsigned n = (unsigned)count < (unsigned)dlen ?
				     (unsigned)count :
				     (unsigned)dlen;
		rx_read(sk, buf, n);
		if (n < (unsigned)dlen) {
			char tmp;
			unsigned rem = (unsigned)dlen - n;
			while (rem--)
				rx_read(sk, &tmp, 1);
		}
		spinlock_unlock(&sk->rxbuf_lock);
		return (ssize_t)n;
	}

	/* SOCK_STREAM */
	spinlock_lock(&sk->rxbuf_lock);
	while (rx_used(sk) == 0) {
		spinlock_unlock(&sk->rxbuf_lock);
		if (sk->err)
			return sk->err;
		if (sk->state == SS_DISCONNECTING)
			return 0;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sock_wait(sk, deadline);
		spinlock_lock(&sk->rxbuf_lock);
	}
	unsigned n = rx_read(sk, buf, (unsigned)count);
	spinlock_unlock(&sk->rxbuf_lock);
	return (ssize_t)n;
}

static ssize_t sock_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	(void)pos;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	if (sk->domain == AF_UNIX)
		return sock_unix_read(sk, buf, count);

	if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW) {
		unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->err)
				return sk->err;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			sock_wait(sk, deadline);
		}
		u16_t dlen;
		rx_read(sk, &dlen, sizeof(dlen));
		unsigned n = (unsigned)count < (unsigned)dlen ?
				     (unsigned)count :
				     (unsigned)dlen;
		rx_read(sk, buf, n);
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
			return 0;
		if (sk->state == SS_UNCONNECTED)
			return -ENOTCONN;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		sock_wait(sk, deadline);
	}
	unsigned n = rx_read(sk, buf, (unsigned)count);
	return (ssize_t)n;
}

static ssize_t sock_unix_write(mos_sock *sk, const void *buf, size_t count)
{
	mos_sock *unix_peer = sk->unix_peer;
	if (!unix_peer)
		return -EPIPE;
	spinlock_lock(&unix_peer->rxbuf_lock);
	if (sk->type == SOCK_DGRAM) {
		u16_t dlen = (u16_t)count;
		if (rx_free(unix_peer) < sizeof(dlen) + (unsigned)count) {
			spinlock_unlock(&unix_peer->rxbuf_lock);
			return -ENOBUFS;
		}
		rx_write(unix_peer, &dlen, sizeof(dlen));
	}
	unsigned n = rx_write(unix_peer, buf, (unsigned)count);
	spinlock_unlock(&unix_peer->rxbuf_lock);
	sock_wakeup(unix_peer);
	return (ssize_t)n;
}

static ssize_t sock_write(file *fp, const void *buf, size_t count, loff_t *pos)
{
	(void)pos;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	if (sk->err)
		return sk->err;

	if (sk->domain == AF_UNIX)
		return sock_unix_write(sk, buf, count);

	if (sk->type == SOCK_RAW) {
		if (sk->hdrincl)
			return (ssize_t)raw_send_hdrincl(buf, (unsigned)count);
		if (sk->state != SS_CONNECTED || !sk->raw)
			return -ENOTCONN;
		ip_addr_t ip;
		ip4_addr_set_u32(ip_2_ip4(&ip), sk->peer.sin_addr.s_addr);
		struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)count, PBUF_RAM);
		if (!p)
			return -ENOBUFS;
		memcpy(p->payload, buf, count);
		err_t e = raw_sendto(sk->raw, p, &ip);
		pbuf_free(p);
		return e == ERR_OK ? (ssize_t)count : -EIO;
	}

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

	if (sk->domain == AF_UNIX) {
		mos_sock *unix_peer = sk->unix_peer;
		if (unix_peer) {
			unix_peer->unix_peer = NULL;
			unix_peer->state = SS_DISCONNECTING;
			sock_wakeup(unix_peer);
		}
	} else if (sk->type == SOCK_RAW) {
		if (sk->raw)
			raw_remove(sk->raw);
	} else if (sk->type == SOCK_DGRAM) {
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

/* ── helpers for filling ifreq fields ───────────────────────────────────── */

static void fill_ifr_sin(struct ifreq *ifr, uint32_t addr_nbo)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = addr_nbo;
}

static void fill_eth0_ifreq(struct ifreq *ifr, struct netif *nif, unsigned cmd)
{
	sprintf(ifr->ifr_name, "%c%c%u", nif->name[0], nif->name[1],
		(unsigned)nif->num);

	switch (cmd) {
	case SIOCGIFFLAGS: {
		short f = IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST;
		ifr->ifr_flags = f;
		break;
	}
	case SIOCGIFADDR:
	case SIOCGIFCONF:
		fill_ifr_sin(ifr, ip4_addr_get_u32(netif_ip4_addr(nif)));
		break;
	case SIOCGIFNETMASK:
		fill_ifr_sin(ifr, ip4_addr_get_u32(netif_ip4_netmask(nif)));
		break;
	case SIOCGIFBRDADDR: {
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
		ifr->ifr_ifindex = (int)(nif->num + 1);
		break;
	case SIOCGIFMETRIC:
		ifr->ifr_metric = 0;
		break;
	default:
		break;
	}
}

static void fill_lo_ifreq(struct ifreq *ifr, unsigned cmd)
{
	strncpy(ifr->ifr_name, "lo", IFNAMSIZ);

	struct netif *lo = netif_find("lo0");

	switch (cmd) {
	case SIOCGIFFLAGS:
		ifr->ifr_flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
		break;
	case SIOCGIFADDR:
	case SIOCGIFCONF: {
		uint32_t addr = lo ? ip4_addr_get_u32(netif_ip4_addr(lo)) :
				     lwip_htonl(0x7F000001UL);
		fill_ifr_sin(ifr, addr);
		break;
	}
	case SIOCGIFNETMASK: {
		uint32_t mask = lo ? ip4_addr_get_u32(netif_ip4_netmask(lo)) :
				     lwip_htonl(0xFF000000UL);
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = mask;
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
		ifr->ifr_ifindex = lo ? (int)(lo->num + 1) : 1;
		break;
	case SIOCGIFMETRIC:
		ifr->ifr_metric = 0;
		break;
	default:
		break;
	}
}

/* ── sock_ioctl ──────────────────────────────────────────────────────────── */

static int sock_ioctl(file *fp, unsigned cmd, void *arg)
{
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	switch (cmd) {
	case SIOCGSTAMP:
		*(struct timeval *)arg = sk->rx_stamp;
		return 0;

	case FIONREAD:
		*(int *)arg = (int)rx_used(sk);
		return 0;

	case FIONBIO:
		return 0;

	case SIOCGIFCONF: {
		struct ifconf *ifc = (struct ifconf *)arg;
		struct ifreq *req = ifc->ifc_req;
		int max = ifc->ifc_len / (int)sizeof(struct ifreq);
		int n = 0;

		struct netif *nif = net_get_default_netif();

		if (n < max && nif) {
			memset(&req[n], 0, sizeof(struct ifreq));
			fill_eth0_ifreq(&req[n], nif, SIOCGIFCONF);
			n++;
		}
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

static int sock_poll(file *fp, unsigned type)
{
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	switch (type) {
	case FS_POLL_READ:
		if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW)
			return rx_used(sk) >= sizeof(u16_t) ? 0 : -1;
		if (rx_used(sk) > 0)
			return 0;
		if (sk->state == SS_DISCONNECTING)
			return 0;
		if (sk->accept_tail != sk->accept_head)
			return 0;
		return -1;

	case FS_POLL_WRITE:
		if (sk->domain == AF_UNIX)
			return sk->unix_peer ? 0 : -1;
		if (sk->type == SOCK_STREAM)
			return sk->state == SS_CONNECTED ? 0 : -1;
		return 0;

	case FS_POLL_EXCEPT:
		return sk->err ? 0 : -1;
	}
	return -1;
}

static const file_operations sock_fops = {
	.read = sock_read,
	.write = sock_write,
	.ioctl = sock_ioctl,
	.poll = sock_poll,
	.release = sock_release,
};

/* ── FD helpers ──────────────────────────────────────────────────────────── */

int sock_to_fd(mos_sock *sk)
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

mos_sock *fd_to_sock(int fd)
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
