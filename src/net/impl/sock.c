/*
 * sock.c — MOS socket core: ring buffer, blocking helpers,
 *          file operations, ioctl, and fd/sock helpers.
 */
#include <net/core.h>
#include <net/sock.h>
#include <net/net.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <device/time.h>
#include <ps/ps.h>
#include <errno.h>
#include <lib/command.h>
#include <macro.h>

#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/raw.h>
#include <lwip/netif.h>
#include <lwip/ip4_addr.h>
#include <lwip/pbuf.h>

/* ── Ring-buffer helpers ─────────────────────────────────────────────────── */

unsigned sock_default_rxbuf_size(int domain, int type)
{
	if (domain != AF_UNIX)
		return SOCK_RXBUF_INET_SIZE;
	return type == SOCK_STREAM ? SOCK_RXBUF_UNIX_STREAM_SIZE :
				     SOCK_RXBUF_UNIX_SIZE;
}

/* A zero socket timeout means an unlimited wait. Keep zero as the
 * deadline sentinel rather than adding it to the current clock. */
unsigned long long sock_recv_deadline(const mos_sock *sk)
{
	return sk->recv_timeout_ms ? time_now_ms() + sk->recv_timeout_ms : 0;
}

unsigned long long sock_send_deadline(const mos_sock *sk)
{
	return sk->send_timeout_ms ? time_now_ms() + sk->send_timeout_ms : 0;
}

int sock_deadline_expired(unsigned long long deadline)
{
	return deadline && time_now_ms() >= deadline;
}

int sock_alloc_rxbuf(mos_sock *sk, unsigned size)
{
	if (!sk || size == 0)
		return -EINVAL;
	if ((size & (size - 1)) != 0)
		return -EINVAL;

	sk->rxbuf = zalloc(size);
	if (!sk->rxbuf)
		return -ENOMEM;
	sk->rxbuf_size = size;
	sk->rx_head = 0;
	sk->rx_tail = 0;
	return 0;
}

void sock_destroy(mos_sock *sk)
{
	if (!sk)
		return;
	free(sk->rxbuf);
	free(sk);
}

unsigned rx_used(const mos_sock *sk)
{
	if (!sk || sk->rxbuf_size == 0)
		return 0;
	return (sk->rx_tail - sk->rx_head) & (sk->rxbuf_size - 1);
}

unsigned rx_free(const mos_sock *sk)
{
	if (!sk || sk->rxbuf_size == 0)
		return 0;
	return (sk->rxbuf_size - 1) - rx_used(sk);
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
	unsigned long long deadline = sock_send_deadline(sk);

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
			if (sock_deadline_expired(deadline))
				return done ? (ssize_t)done : -EAGAIN;
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
		if (sock_deadline_expired(deadline))
			return done ? (ssize_t)done : -EAGAIN;

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
		unsigned pos = sk->rx_tail & (sk->rxbuf_size - 1);
		unsigned first = sk->rxbuf_size - pos;
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
		unsigned pos = sk->rx_head & (sk->rxbuf_size - 1);
		unsigned first = sk->rxbuf_size - pos;
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
	file *async_fp;
	int owner = 0;
	int sig = SIGIO;
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
	async_fp = sk->async_file;
	if (async_fp && (async_fp->f_flag & FASYNC) && async_fp->f_owner) {
		owner = async_fp->f_owner;
		if (async_fp->f_sigio > 0)
			sig = async_fp->f_sigio;
	}
	spinlock_unlock(&sk->wait_lock, irq);

	if (owner)
		ps_send_signal_owner(owner, sig);
}

/* Block the current task on sk until woken by sock_wakeup or the deadline.
 * Returns -1 if a deliverable signal is pending after waking, 0 otherwise. */
static void sock_cancel_wait(void *opaque)
{
	sock_waiter *waiter = opaque;
	int irq;
	spinlock_lock(&waiter->sk->wait_lock, &irq);
	sock_waiter_dequeue(waiter);
	spinlock_unlock(&waiter->sk->wait_lock, irq);
}

int sock_wait(mos_sock *sk, unsigned long long deadline)
{
	unsigned long long now;
	sock_waiter waiter;
	int irq;
	task_struct *cur = CURRENT_TASK();

	now = deadline ? time_now_ms() : 0;
	if (deadline && now >= deadline)
		return 0;

	list_init(&waiter.node);
	waiter.task = cur;
	waiter.sk = sk;
	waiter.queued = 0;

	spinlock_lock(&sk->wait_lock, &irq);
	if (ps_interrupting_signals(cur)) {
		spinlock_unlock(&sk->wait_lock, irq);
		return -1;
	}
	sock_waiter_queue(&sk->waiters, &waiter);
	cur->io_wait = &waiter;
	cur->cancel_io_wait = sock_cancel_wait;
	now = deadline ? time_now_ms() : 0;
	if (deadline && now >= deadline) {
		sock_waiter_dequeue(&waiter);
		cur->io_wait = NULL;
		cur->cancel_io_wait = NULL;
		spinlock_unlock(&sk->wait_lock, irq);
		return 0;
	}
	if (ps_prepare_interruptible_wait(
		    cur, NULL, deadline ? (unsigned)(deadline - now) : 0,
		    __func__) < 0) {
		sock_waiter_dequeue(&waiter);
		cur->io_wait = NULL;
		cur->cancel_io_wait = NULL;
		spinlock_unlock(&sk->wait_lock, irq);
		return -1;
	}
	spinlock_unlock(&sk->wait_lock, irq);

	/* Publish stack work before sleeping inside a network guard. */
	net_service_update();
	task_sched();
	ps_finish_timed_wait(cur);

	spinlock_lock(&sk->wait_lock, &irq);
	sock_waiter_dequeue(&waiter);
	cur->io_wait = NULL;
	cur->cancel_io_wait = NULL;
	spinlock_unlock(&sk->wait_lock, irq);

	if (ps_interrupting_signals(cur))
		return -1;
	return 0;
}

/* ── Socket file operations ──────────────────────────────────────────────── */

static ssize_t sock_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	NET_CORE_GUARD;
	(void)pos;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;
	int nonblock = sock_file_nonblock(fp);

	if (count == 0 && sk->type == SOCK_STREAM)
		return 0;

	if (sk->domain == AF_UNIX)
		return unix_read(fp, sk, buf, count);

	if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW) {
		unsigned long long deadline = sock_recv_deadline(sk);
		while (rx_used(sk) < sizeof(u16_t)) {
			if (sk->err)
				return sk->err;
			if (nonblock)
				return -EAGAIN;
			if (sock_deadline_expired(deadline))
				return -EAGAIN;
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
	unsigned long long deadline = sock_recv_deadline(sk);
	while (rx_used(sk) == 0) {
		if (sk->err)
			return sk->err;
		if (sk->state == SS_DISCONNECTING)
			return 0;
		if (sk->state == SS_UNCONNECTED)
			return -ENOTCONN;
		if (nonblock)
			return -EAGAIN;
		if (sock_deadline_expired(deadline))
			return -EAGAIN;
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
	NET_CORE_GUARD;
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

/* Detach callbacks before destroying an unexposed accepted socket. */
void sock_tcp_abort(mos_sock *sk)
{
	struct tcp_pcb *pcb = sk->tcp;
	if (!pcb)
		return;
	sk->tcp = NULL;
	tcp_arg(pcb, NULL);
	tcp_recv(pcb, NULL);
	tcp_sent(pcb, NULL);
	tcp_err(pcb, NULL);
	tcp_abort(pcb);
}

static int sock_release(file *fp)
{
	NET_CORE_GUARD;
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
		/* Closing a listener also owns all not-yet-accepted sockets. */
		while (sk->accept_head != sk->accept_tail) {
			mos_sock *pending = sk->accept_queue[sk->accept_head];
			sk->accept_queue[sk->accept_head] = NULL;
			sk->accept_head =
				(sk->accept_head + 1) % SOCK_ACCEPT_BACKLOG;
			sock_tcp_abort(pending);
			sock_destroy(pending);
		}
		if (sk->tcp) {
			struct tcp_pcb *pcb = sk->tcp;
			err_t err;

			sk->tcp = NULL;
			tcp_arg(pcb, NULL);
			if (pcb->state == LISTEN) {
				tcp_accept(pcb, NULL);
			} else {
				tcp_recv(pcb, NULL);
				tcp_sent(pcb, NULL);
				tcp_err(pcb, NULL);
			}
			err = tcp_close(pcb);
			if (err != ERR_OK)
				tcp_abort(pcb);
		}
	}

	sock_destroy(sk);
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

static int fill_eth0_ifreq_siocgifflags(void *context __attribute__((unused)),
					unsigned cmd __attribute__((unused)),
					void *arg)
{
	struct ifreq *ifr = arg;
	short f = IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST;
	ifr->ifr_flags = f;
	return 0;
}

static int fill_eth0_ifreq_siocgifaddr(void *context __attribute__((unused)),
				       unsigned cmd __attribute__((unused)),
				       void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *nif = context;
	fill_ifr_sin(ifr, ip4_addr_get_u32(netif_ip4_addr(nif)));
	return 0;
}

static int fill_eth0_ifreq_siocgifnetmask(void *context __attribute__((unused)),
					  unsigned cmd __attribute__((unused)),
					  void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *nif = context;
	fill_ifr_sin(ifr, ip4_addr_get_u32(netif_ip4_netmask(nif)));
	return 0;
}

static int fill_eth0_ifreq_siocgifbrdaddr(void *context __attribute__((unused)),
					  unsigned cmd __attribute__((unused)),
					  void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *nif = context;
	uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(nif));
	uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(nif));
	fill_ifr_sin(ifr, ip | ~mask);
	return 0;
}

static int fill_eth0_ifreq_siocgifhwaddr(void *context __attribute__((unused)),
					 unsigned cmd __attribute__((unused)),
					 void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *nif = context;
	struct sockaddr *sa = &ifr->ifr_hwaddr;
	memset(sa, 0, sizeof(*sa));
	sa->sa_family = ARPHRD_ETHER;
	memcpy(sa->sa_data, nif->hwaddr, 6);
	return 0;
}

static int fill_eth0_ifreq_siocgifmtu(void *context __attribute__((unused)),
				      unsigned cmd __attribute__((unused)),
				      void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *nif = context;
	ifr->ifr_mtu = nif->mtu;
	return 0;
}

static int fill_eth0_ifreq_siocgifindex(void *context __attribute__((unused)),
					unsigned cmd __attribute__((unused)),
					void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *nif = context;
	ifr->ifr_ifindex = (int)(nif->num + 1);
	return 0;
}

static int fill_eth0_ifreq_siocgifmetric(void *context __attribute__((unused)),
					 unsigned cmd __attribute__((unused)),
					 void *arg)
{
	struct ifreq *ifr = arg;
	ifr->ifr_metric = 0;
	return 0;
}

static const command_operation fill_eth0_ifreq_operations[256] = {
	[SIOCGIFFLAGS & 255] = { SIOCGIFFLAGS, fill_eth0_ifreq_siocgifflags },
	[SIOCGIFADDR & 255] = { SIOCGIFADDR, fill_eth0_ifreq_siocgifaddr },
	[SIOCGIFCONF & 255] = { SIOCGIFCONF, fill_eth0_ifreq_siocgifaddr },
	[SIOCGIFNETMASK & 255] = { SIOCGIFNETMASK,
				   fill_eth0_ifreq_siocgifnetmask },
	[SIOCGIFBRDADDR & 255] = { SIOCGIFBRDADDR,
				   fill_eth0_ifreq_siocgifbrdaddr },
	[SIOCGIFHWADDR & 255] = { SIOCGIFHWADDR,
				  fill_eth0_ifreq_siocgifhwaddr },
	[SIOCGIFMTU & 255] = { SIOCGIFMTU, fill_eth0_ifreq_siocgifmtu },
	[SIOCGIFINDEX & 255] = { SIOCGIFINDEX, fill_eth0_ifreq_siocgifindex },
	[SIOCGIFMETRIC & 255] = { SIOCGIFMETRIC,
				  fill_eth0_ifreq_siocgifmetric },
};
static const command_operation *const fill_eth0_ifreq_groups[256] = {
	[(SIOCGIFFLAGS >> 8) & 255] = fill_eth0_ifreq_operations,
};

static void fill_eth0_ifreq(struct ifreq *ifr, struct netif *nif, unsigned cmd)
{
	sprintf(ifr->ifr_name, "%c%c%u", nif->name[0], nif->name[1],
		(unsigned)nif->num);

	command_dispatch(fill_eth0_ifreq_groups, nif, cmd, ifr, 0);
}

static int fill_lo_ifreq_siocgifflags(void *context __attribute__((unused)),
				      unsigned cmd __attribute__((unused)),
				      void *arg)
{
	struct ifreq *ifr = arg;
	ifr->ifr_flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
	return 0;
}

static int fill_lo_ifreq_siocgifaddr(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *lo = context;
	uint32_t addr = lo ? ip4_addr_get_u32(netif_ip4_addr(lo)) :
			     lwip_htonl(0x7F000001UL);
	fill_ifr_sin(ifr, addr);
	return 0;
}

static int fill_lo_ifreq_siocgifnetmask(void *context __attribute__((unused)),
					unsigned cmd __attribute__((unused)),
					void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *lo = context;
	uint32_t mask = lo ? ip4_addr_get_u32(netif_ip4_netmask(lo)) :
			     lwip_htonl(0xFF000000UL);
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = mask;
	return 0;
}

static int fill_lo_ifreq_siocgifbrdaddr(void *context __attribute__((unused)),
					unsigned cmd __attribute__((unused)),
					void *arg)
{
	struct ifreq *ifr = arg;
	fill_ifr_sin(ifr, 0);
	return 0;
}

static int fill_lo_ifreq_siocgifhwaddr(void *context __attribute__((unused)),
				       unsigned cmd __attribute__((unused)),
				       void *arg)
{
	struct ifreq *ifr = arg;
	memset(&ifr->ifr_hwaddr, 0, sizeof(struct sockaddr));
	ifr->ifr_hwaddr.sa_family = ARPHRD_ETHER;
	return 0;
}

static int fill_lo_ifreq_siocgifmtu(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *arg)
{
	struct ifreq *ifr = arg;
	ifr->ifr_mtu = 65536;
	return 0;
}

static int fill_lo_ifreq_siocgifindex(void *context __attribute__((unused)),
				      unsigned cmd __attribute__((unused)),
				      void *arg)
{
	struct ifreq *ifr = arg;
	struct netif *lo = context;
	ifr->ifr_ifindex = lo ? (int)(lo->num + 1) : 1;
	return 0;
}

static int fill_lo_ifreq_siocgifmetric(void *context __attribute__((unused)),
				       unsigned cmd __attribute__((unused)),
				       void *arg)
{
	struct ifreq *ifr = arg;
	ifr->ifr_metric = 0;
	return 0;
}

static const command_operation fill_lo_ifreq_operations[256] = {
	[SIOCGIFFLAGS & 255] = { SIOCGIFFLAGS, fill_lo_ifreq_siocgifflags },
	[SIOCGIFADDR & 255] = { SIOCGIFADDR, fill_lo_ifreq_siocgifaddr },
	[SIOCGIFCONF & 255] = { SIOCGIFCONF, fill_lo_ifreq_siocgifaddr },
	[SIOCGIFNETMASK & 255] = { SIOCGIFNETMASK,
				   fill_lo_ifreq_siocgifnetmask },
	[SIOCGIFBRDADDR & 255] = { SIOCGIFBRDADDR,
				   fill_lo_ifreq_siocgifbrdaddr },
	[SIOCGIFHWADDR & 255] = { SIOCGIFHWADDR, fill_lo_ifreq_siocgifhwaddr },
	[SIOCGIFMTU & 255] = { SIOCGIFMTU, fill_lo_ifreq_siocgifmtu },
	[SIOCGIFINDEX & 255] = { SIOCGIFINDEX, fill_lo_ifreq_siocgifindex },
	[SIOCGIFMETRIC & 255] = { SIOCGIFMETRIC, fill_lo_ifreq_siocgifmetric },
};
static const command_operation *const fill_lo_ifreq_groups[256] = {
	[(SIOCGIFFLAGS >> 8) & 255] = fill_lo_ifreq_operations,
};

static void fill_lo_ifreq(struct ifreq *ifr, unsigned cmd)
{
	strncpy(ifr->ifr_name, "lo", IFNAMSIZ);

	struct netif *lo = netif_find("lo0");

	command_dispatch(fill_lo_ifreq_groups, lo, cmd, ifr, 0);
}

/* ── sock_ioctl ──────────────────────────────────────────────────────────── */

static int sock_ioctl_siocgstamp(void *context __attribute__((unused)),
				 unsigned cmd __attribute__((unused)),
				 void *arg __attribute__((unused)))
{
	file *fp = context;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;
#if MOS_HAS_NATIVE_USER
	if (current->user->abi == MOS_ABI_AMD64) {
		int64_t *wire = arg;
		wire[0] = sk->rx_stamp.tv_sec;
		wire[1] = sk->rx_stamp.tv_usec;
		return 0;
	}
#endif
	*(struct timeval *)arg = sk->rx_stamp;
	return 0;
}

static int sock_ioctl_fionread(void *context __attribute__((unused)),
			       unsigned cmd __attribute__((unused)),
			       void *arg __attribute__((unused)))
{
	file *fp = context;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;
	*(int *)arg = (int)rx_used(sk);
	return 0;
}

static int sock_ioctl_fionbio(void *context __attribute__((unused)),
			      unsigned cmd __attribute__((unused)),
			      void *arg __attribute__((unused)))
{
	file *fp = context;
	if (!arg)
		return -EFAULT;
	if (*(const int *)arg)
		fp->f_flag |= O_NONBLOCK;
	else
		fp->f_flag &= ~O_NONBLOCK;
	return 0;
}

static int sock_ioctl_siocgifconf(void *context __attribute__((unused)),
				  unsigned cmd __attribute__((unused)),
				  void *arg __attribute__((unused)))
{
	struct ifconf *ifc = (struct ifconf *)arg;
	if (!ifc)
		return -EFAULT;
	struct ifreq *req = ifc->ifc_req;
	struct netif *nif = net_get_default_netif();
	/* Linux permits a NULL buffer to query the required byte count. */
	if (!req) {
		ifc->ifc_len = (nif ? 2 : 1) * sizeof(struct ifreq);
		return 0;
	}
	if (ifc->ifc_len < 0)
		return -EINVAL;
	int max = ifc->ifc_len / (int)sizeof(struct ifreq);
	int n = 0;

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

static int sock_ioctl_siocgifflags(void *context __attribute__((unused)),
				   unsigned cmd __attribute__((unused)),
				   void *arg __attribute__((unused)))
{
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

static const command_operation socket_network_commands[256] = {
	[SIOCGSTAMP & 255] = { SIOCGSTAMP, sock_ioctl_siocgstamp },
	[SIOCGIFCONF & 255] = { SIOCGIFCONF, sock_ioctl_siocgifconf },
	[SIOCGIFFLAGS & 255] = { SIOCGIFFLAGS, sock_ioctl_siocgifflags },
	[SIOCGIFADDR & 255] = { SIOCGIFADDR, sock_ioctl_siocgifflags },
	[SIOCGIFNETMASK & 255] = { SIOCGIFNETMASK, sock_ioctl_siocgifflags },
	[SIOCGIFBRDADDR & 255] = { SIOCGIFBRDADDR, sock_ioctl_siocgifflags },
	[SIOCGIFHWADDR & 255] = { SIOCGIFHWADDR, sock_ioctl_siocgifflags },
	[SIOCGIFMTU & 255] = { SIOCGIFMTU, sock_ioctl_siocgifflags },
	[SIOCGIFINDEX & 255] = { SIOCGIFINDEX, sock_ioctl_siocgifflags },
	[SIOCGIFMETRIC & 255] = { SIOCGIFMETRIC, sock_ioctl_siocgifflags },
};

static const command_operation socket_file_commands[256] = {
	[FIONREAD & 255] = { FIONREAD, sock_ioctl_fionread },
	[FIONBIO & 255] = { FIONBIO, sock_ioctl_fionbio },
};

static const command_operation *const socket_command_groups[256] = {
	[(SIOCGSTAMP >> 8) & 255] = socket_network_commands,
	[(FIONREAD >> 8) & 255] = socket_file_commands,
};

static int sock_ioctl(file *fp, unsigned cmd, void *arg)
{
	NET_CORE_GUARD;

	return command_dispatch(socket_command_groups, fp, cmd, arg, -ENOTTY);
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
	NET_CORE_GUARD;
	mos_sock *sk = (mos_sock *)fp->f_inode->i_private;
	unsigned ready = 0;

	if (events & FS_POLL_READ) {
		if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW)
			ready |= rx_used(sk) >= sizeof(u16_t) ? FS_POLL_READ :
								0;
		if (rx_used(sk) > 0)
			ready |= FS_POLL_READ;
		if (sk->state == SS_DISCONNECTING ||
		    (sk->domain == AF_UNIX &&
		     (sk->unix_shutdown & UNIX_SHUT_RD)))
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
			if (sk->unix_shutdown & UNIX_SHUT_WR) {
				ready |= FS_POLL_WRITE;
			} else if (sk->unix_peer) {
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
	if ((events & FS_POLL_HUP) &&
	    (sk->state == SS_DISCONNECTING ||
	     (sk->domain == AF_UNIX &&
	      sk->unix_shutdown == (UNIX_SHUT_RD | UNIX_SHUT_WR))))
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
	sk->async_file = fp;

	int fd = fs_install_fd(fp, O_RDWR);
	if (fd < 0) {
		sk->async_file = NULL;
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
