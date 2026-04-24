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

static int sock_file_nonblock(file *fp)
{
	return (fp->f_flag & O_NONBLOCK) != 0;
}

static int sock_getattr(file *fp, struct stat *s);

static ssize_t sock_tcp_stream_write(file *fp, mos_sock *sk, const void *buf,
				     size_t count)
{
	const char *p = (const char *)buf;
	size_t done = 0;
	int nonblock = sock_file_nonblock(fp);
	unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;

	while (done < count) {
		size_t remain = count - done;
		u16_t avail;
		u16_t chunk;
		err_t e;

		if (sk->err)
			return done ? (ssize_t)done : sk->err;
		if (sk->state != SS_CONNECTED && sk->state != SS_DISCONNECTING)
			return done ? (ssize_t)done : -ENOTCONN;

		avail = sk->tcp ? tcp_sndbuf(sk->tcp) : 0;
		if (avail == 0) {
			if (nonblock)
				return done ? (ssize_t)done : -EAGAIN;
			if (time_now_ms() > deadline)
				return done ? (ssize_t)done : -ETIMEDOUT;
			tcp_output(sk->tcp);
			if (sock_wait(sk, deadline) < 0)
				return done ? (ssize_t)done : -EINTR;
			continue;
		}

		chunk = avail;
		if (chunk > remain)
			chunk = (u16_t)remain;
		e = tcp_write(sk->tcp, p + done, chunk, TCP_WRITE_FLAG_COPY);
		if (e == ERR_OK) {
			done += chunk;
			tcp_output(sk->tcp);
			continue;
		}

		if (e != ERR_MEM)
			return done ? (ssize_t)done : -EIO;
		if (nonblock)
			return done ? (ssize_t)done : -EAGAIN;
		if (time_now_ms() > deadline)
			return done ? (ssize_t)done : -ETIMEDOUT;

		tcp_output(sk->tcp);
		if (sock_wait(sk, deadline) < 0)
			return done ? (ssize_t)done : -EINTR;
	}

	return (ssize_t)done;
}

/* Write up to len bytes from src; returns bytes actually written.
 * Split into at most two memcpy calls to handle the ring wrap. */
unsigned rx_write(mos_sock *sk, const void *src, unsigned len)
{
	unsigned avail = rx_free(sk);
	unsigned n = len < avail ? len : avail;

	if (n > 0) {
		unsigned pos = sk->rx_tail & (SOCK_RXBUF_SIZE - 1);
		unsigned first = SOCK_RXBUF_SIZE - pos;
		if (first > n)
			first = n;
		memcpy(sk->rxbuf + pos, src, first);
		if (first < n)
			memcpy(sk->rxbuf, (const char *)src + first, n - first);
		sk->rx_tail += n;
		us_to_timeval(time_wall_us(), &sk->rx_stamp);
	}
	return n;
}

/* Read up to len bytes into dst; returns bytes actually read.
 * Split into at most two memcpy calls to handle the ring wrap. */
unsigned rx_read(mos_sock *sk, void *dst, unsigned len)
{
	unsigned avail = rx_used(sk);
	unsigned n = len < avail ? len : avail;

	if (n > 0) {
		unsigned pos = sk->rx_head & (SOCK_RXBUF_SIZE - 1);
		unsigned first = SOCK_RXBUF_SIZE - pos;
		if (first > n)
			first = n;
		memcpy(dst, sk->rxbuf + pos, first);
		if (first < n)
			memcpy((char *)dst + first, sk->rxbuf, n - first);
		sk->rx_head += n;
	}
	return n;
}

unsigned rx_iov_write(mos_sock *sk, const struct iovec *iov, size_t iovlen)
{
	size_t i;
	unsigned written = 0;

	for (i = 0; i < iovlen; i++)
		written +=
			rx_write(sk, iov[i].iov_base, (unsigned)iov[i].iov_len);
	return written;
}

unsigned rx_iov_read(mos_sock *sk, const struct iovec *iov, size_t iovlen,
		     unsigned limit)
{
	size_t i;
	unsigned delivered = 0;
	unsigned remaining = limit;

	for (i = 0; i < iovlen && remaining > 0; i++) {
		unsigned n = (unsigned)iov[i].iov_len < remaining ?
				     (unsigned)iov[i].iov_len :
				     remaining;
		rx_read(sk, iov[i].iov_base, n);
		delivered += n;
		remaining -= n;
	}
	return delivered;
}

void rx_discard(mos_sock *sk, unsigned len)
{
	while (len > 0) {
		char tmp;
		rx_read(sk, &tmp, 1);
		len--;
	}
}

/* ── Blocking helpers ────────────────────────────────────────────────────── */

static void sock_waiter_queue(list_entry *head, sock_waiter *waiter)
{
	list_insert_tail(head, &waiter->node);
	waiter->queued = 1;
}

static void sock_waiter_dequeue(sock_waiter *waiter)
{
	if (!waiter->queued)
		return;
	list_remove_entry(&waiter->node);
	list_init(&waiter->node);
	waiter->queued = 0;
}

/* Wake the task currently blocked on sk, if any.
 * Also wakes any poll/select waiter registered via poll_wait.
 * Safe to call from lwIP callbacks (NIC IRQ context). */
void sock_wakeup(mos_sock *sk)
{
	list_entry *entry;
	int irq;

	spinlock_lock(&sk->wait_lock, &irq);
	while (!list_is_empty(&sk->waiters)) {
		sock_waiter *waiter = container_of(
			list_remove_head(&sk->waiters), sock_waiter, node);
		list_init(&waiter->node);
		waiter->queued = 0;
		ps_put_to_ready_queue(waiter->task);
	}

	entry = sk->poll_waiters.next;
	while (entry != &sk->poll_waiters) {
		sock_waiter *waiter = container_of(entry, sock_waiter, node);
		entry = entry->next;
		ps_put_to_ready_queue(waiter->task);
	}
	spinlock_unlock(&sk->wait_lock, irq);
}

/* Block the current task on sk until woken by sock_wakeup or the deadline.
 * Returns -1 if a deliverable signal is pending after waking, 0 otherwise. */
int sock_wait(mos_sock *sk, unsigned long deadline)
{
	unsigned long now = time_now_ms();
	sock_waiter waiter;
	int irq;
	if (now >= deadline)
		return 0;
	task_struct *cur = CURRENT_TASK();

	list_init(&waiter.node);
	waiter.task = cur;
	waiter.sk = sk;
	waiter.queued = 0;

	spinlock_lock(&sk->wait_lock, &irq);
	sock_waiter_queue(&sk->waiters, &waiter);
	spinlock_unlock(&sk->wait_lock, irq);

	time_wait((unsigned)(deadline - now));

	spinlock_lock(&sk->wait_lock, &irq);
	sock_waiter_dequeue(&waiter);
	spinlock_unlock(&sk->wait_lock, irq);

	if (cur->signal && (cur->signal->sig_pending & ~cur->signal->sig_mask))
		return -1;
	return 0;
}

/* ── Socket file operations ──────────────────────────────────────────────── */

static ssize_t sock_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	(void)pos;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;
	int nonblock = sock_file_nonblock(fp);

	if (sk->domain == AF_UNIX)
		return unix_read(fp, sk, buf, count);

	if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW) {
		unsigned long deadline = time_now_ms() + SOCK_TIMEOUT_MS;
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->err)
				return sk->err;
			if (time_now_ms() > deadline)
				return -ETIMEDOUT;
			if (sock_wait(sk, deadline) < 0)
				return -EINTR;
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
		if (nonblock)
			return -EAGAIN;
		if (time_now_ms() > deadline)
			return -ETIMEDOUT;
		if (sock_wait(sk, deadline) < 0)
			return -EINTR;
	}
	unsigned n = rx_read(sk, buf, (unsigned)count);
	if (n > 0 && sk->tcp)
		tcp_recved(sk->tcp, n);
	return (ssize_t)n;
}

static ssize_t sock_write(file *fp, const void *buf, size_t count, loff_t *pos)
{
	(void)pos;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	if (sk->err)
		return sk->err;

	if (sk->domain == AF_UNIX)
		return unix_write(fp, sk, buf, count);

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
	return sock_tcp_stream_write(fp, sk, buf, count);
}

static int sock_release(file *fp)
{
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;

	if (sk->domain == AF_UNIX) {
		int irq;
		spinlock_lock(&sk->rxbuf_lock, &irq);
		unix_drop_passfds(sk);
		spinlock_unlock(&sk->rxbuf_lock, irq);
		unix_release(sk);
	} else if (sk->type == SOCK_RAW) {
		if (sk->raw)
			raw_remove(sk->raw);
	} else if (sk->type == SOCK_DGRAM) {
		if (sk->udp)
			udp_remove(sk->udp);
	} else {
		if (sk->tcp) {
			struct tcp_pcb *pcb = sk->tcp;
			err_t err;

			sk->tcp = NULL;
			tcp_arg(pcb, NULL);
			tcp_recv(pcb, NULL);
			tcp_sent(pcb, NULL);
			tcp_err(pcb, NULL);
			err = tcp_close(pcb);
			if (err != ERR_OK)
				tcp_abort(pcb);
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

static void sock_poll_dereg(void *opaque, task_struct *task)
{
	sock_waiter *waiter = opaque;
	int irq;

	(void)task;
	spinlock_lock(&waiter->sk->wait_lock, &irq);
	sock_waiter_dequeue(waiter);
	spinlock_unlock(&waiter->sk->wait_lock, irq);
	free(waiter);
}

static unsigned sock_poll(file *fp, unsigned events, poll_table *pt)
{
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;
	unsigned ready = 0;

	if (events & FS_POLL_READ) {
		if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW)
			ready |= rx_used(sk) >= sizeof(u16_t) ? FS_POLL_READ :
								0;
		if (rx_used(sk) > 0)
			ready |= FS_POLL_READ;
		if (sk->state == SS_DISCONNECTING)
			ready |= FS_POLL_READ;
		if (sk->domain == AF_UNIX) {
			if (sk->unix_accept_tail != sk->unix_accept_head)
				ready |= FS_POLL_READ;
		} else if (sk->accept_tail != sk->accept_head) {
			ready |= FS_POLL_READ;
		}
	}

	if (events & FS_POLL_WRITE) {
		if (sk->domain == AF_UNIX) {
			if (sk->unix_peer) {
				if (sk->type == SOCK_DGRAM) {
					ready |= rx_free(sk->unix_peer) >=
								 sizeof(u16_t) ?
							 FS_POLL_WRITE :
							 0;
				} else {
					ready |= rx_free(sk->unix_peer) > 0 ?
							 FS_POLL_WRITE :
							 0;
				}
			}
		} else if (sk->type == SOCK_STREAM) {
			if (sk->state == SS_CONNECTED && sk->tcp &&
			    tcp_sndbuf(sk->tcp) > 0)
				ready |= FS_POLL_WRITE;
		} else
			ready |= FS_POLL_WRITE;
	}

	if ((events & FS_POLL_ERR) && sk->err)
		ready |= FS_POLL_ERR;
	if ((events & FS_POLL_HUP) && sk->state == SS_DISCONNECTING)
		ready |= FS_POLL_HUP;

	if (!ready && pt) {
		sock_waiter *waiter = zalloc(sizeof(*waiter));
		int irq;

		if (!waiter) {
			pt->unsupported = 1;
			return ready;
		}

		list_init(&waiter->node);
		waiter->task = pt->task;
		waiter->sk = sk;
		waiter->queued = 0;
		spinlock_lock(&sk->wait_lock, &irq);
		sock_waiter_queue(&sk->poll_waiters, waiter);
		spinlock_unlock(&sk->wait_lock, irq);

		if (poll_table_add(pt, waiter, sock_poll_dereg) < 0) {
			spinlock_lock(&sk->wait_lock, &irq);
			sock_waiter_dequeue(waiter);
			spinlock_unlock(&sk->wait_lock, irq);
			free(waiter);
		}
	}
	return ready;
}

static const file_operations sock_fops = {
	.getattr = sock_getattr,
	.read = sock_read,
	.write = sock_write,
	.ioctl = sock_ioctl,
	.poll = sock_poll,
	.release = sock_release,
};

static int sock_getattr(file *fp, struct stat *s)
{
	(void)fp;

	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFSOCK | 0600;
	s->st_nlink = 1;
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	return 0;
}

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
	fp->f_flag = O_RDWR;

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
	if (!cur->fds[fd])
		return NULL;
	file *fp = cur->fds[fd];
	if (!fp || !fp->f_inode || !S_ISSOCK(fp->f_inode->i_mode))
		return NULL;
	return (mos_sock *)fp->f_inode->i_private;
}
