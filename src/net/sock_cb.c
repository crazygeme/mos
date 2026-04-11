/*
 * sock_cb.c — lwIP callbacks for TCP, UDP, and RAW sockets,
 *             plus IP_HDRINCL raw send helper.
 */
#include <net/sock.h>
#include <lib/klib.h>
#include <errno.h>

#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/raw.h>
#include <lwip/ip.h>
#include <lwip/ip4.h>
#include <lwip/ip4_addr.h>
#include <lwip/pbuf.h>
#include <lwip/netif.h>
#include <lwip/prot/ip4.h>
#include <lwip/prot/ip.h>

/* ── lwIP TCP callbacks ──────────────────────────────────────────────────── */

static err_t tcp_on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
			 err_t err)
{
	mos_sock *sk = (mos_sock *)arg;
	(void)err;

	if (!p) {
		sk->state = SS_DISCONNECTING;
		sock_wakeup(sk);
		return ERR_OK;
	}

	/*
	 * TCP is a byte stream: dropping any tail bytes from a received pbuf
	 * corrupts the stream irrecoverably. If the userspace receive ring
	 * cannot hold the whole segment yet, leave the pbuf unconsumed so lwIP
	 * can retry delivery later instead of partially copying then freeing it.
	 */
	if (rx_free(sk) < (unsigned)p->tot_len)
		return ERR_MEM;

	struct pbuf *q;
	for (q = p; q; q = q->next)
		rx_write(sk, q->payload, (unsigned)q->len);
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	sock_wakeup(sk);
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
	sock_wakeup(sk);
	return ERR_OK;
}

static void tcp_on_err(void *arg, err_t err)
{
	mos_sock *sk = (mos_sock *)arg;
	(void)err;
	sk->tcp = NULL;
	sk->err = -ECONNREFUSED;
	sk->state = SS_UNCONNECTED;
	sock_wakeup(sk);
}

static err_t tcp_on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	mos_sock *sk = (mos_sock *)arg;
	if (err != ERR_OK)
		return err;

	int next = (sk->accept_tail + 1) % SOCK_ACCEPT_BACKLOG;
	if (next == sk->accept_head)
		return ERR_MEM;

	sk->accept_queue[sk->accept_tail] = newpcb;
	sk->accept_tail = next;
	sock_wakeup(sk);
	return ERR_OK;
}

void tcp_setup_callbacks(struct tcp_pcb *pcb, mos_sock *sk)
{
	tcp_arg(pcb, sk);
	tcp_recv(pcb, tcp_on_recv);
	tcp_err(pcb, tcp_on_err);
}

/* ── lwIP UDP callback ───────────────────────────────────────────────────── */

static void udp_on_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
			const ip_addr_t *addr, u16_t port)
{
	mos_sock *sk = (mos_sock *)arg;
	(void)pcb;

	if (!p)
		return;

	sk->rx_src.sin_family = AF_INET;
	sk->rx_src.sin_port = lwip_htons(port);
	sk->rx_src.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(addr));

	sk->rx_dst.s_addr = ip4_addr_get_u32(ip4_current_dest_addr());
	const struct netif *in_nif = ip_current_input_netif();
	sk->rx_ifindex = in_nif ? (int)(in_nif->num + 1) : 0;
	sk->rx_ttl = (int)(unsigned int)IPH_TTL(ip4_current_header());

	u16_t dlen = p->tot_len;
	if (rx_free(sk) >= (unsigned)(sizeof(dlen) + dlen)) {
		rx_write(sk, &dlen, sizeof(dlen));
		struct pbuf *q;
		for (q = p; q; q = q->next)
			rx_write(sk, q->payload, (unsigned)q->len);
	}
	pbuf_free(p);
	sock_wakeup(sk);
}

/* ── lwIP raw (ICMP) callback ────────────────────────────────────────────── */

static u8_t raw_on_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
			const ip_addr_t *addr)
{
	mos_sock *sk = (mos_sock *)arg;
	(void)pcb;

	if (!p)
		return 0;

	/* Apply ICMP_FILTER — return 0 so ICMP can still process the packet */
	{
		const struct ip_hdr *iph = (const struct ip_hdr *)p->payload;
		unsigned ihl = IPH_HL(iph) * 4;
		if (p->len > ihl) {
			uint8_t icmp_type = ((const uint8_t *)p->payload)[ihl];
			if (sk->icmp_filter.data & (1u << icmp_type))
				return 0;
		}
	}

	sk->rx_src.sin_family = AF_INET;
	sk->rx_src.sin_port = 0;
	sk->rx_src.sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(addr));

	sk->rx_dst.s_addr = ip4_addr_get_u32(ip4_current_dest_addr());
	const struct netif *in_nif = ip_current_input_netif();
	sk->rx_ifindex = in_nif ? (int)(in_nif->num + 1) : 0;
	sk->rx_ttl = (int)(unsigned int)IPH_TTL(ip4_current_header());

	u16_t totlen = p->tot_len;

	if (rx_free(sk) >= (unsigned)(sizeof(totlen) + totlen)) {
		rx_write(sk, &totlen, sizeof(totlen));
		struct pbuf *q;
		for (q = p; q; q = q->next)
			rx_write(sk, q->payload, (unsigned)q->len);
	}
	/* Do not free p — return 0 so lwIP's ICMP layer can still process
	 * echo requests and generate replies (required for loopback ping). */
	sock_wakeup(sk);
	return 0;
}

/* ── IP_HDRINCL raw send ─────────────────────────────────────────────────── */

int raw_send_hdrincl(const void *buf, unsigned len)
{
	if (len < sizeof(struct ip_hdr))
		return -EINVAL;

	const struct ip_hdr *iph = (const struct ip_hdr *)buf;
	ip4_addr_t dest;
	dest.addr = iph->dest.addr;

	struct netif *nif = ip4_route(&dest);
	if (!nif)
		return -ENETUNREACH;

	struct pbuf *p = pbuf_alloc(PBUF_LINK, (u16_t)len, PBUF_RAM);
	if (!p)
		return -ENOBUFS;
	memcpy(p->payload, buf, len);

	err_t e = nif->output(nif, p, &dest);
	pbuf_free(p);
	return e == ERR_OK ? (int)len : -EIO;
}

/* ── Setup helpers used by sock_ops.c ────────────────────────────────────── */

err_t sock_tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *ip, u16_t port)
{
	return tcp_connect(pcb, ip, port, tcp_on_connected);
}

void sock_tcp_accept_listen(struct tcp_pcb *pcb, mos_sock *sk)
{
	(void)sk;
	tcp_accept(pcb, tcp_on_accept);
}

void sock_udp_recv_setup(struct udp_pcb *pcb, mos_sock *sk)
{
	udp_recv(pcb, udp_on_recv, sk);
}

void sock_raw_recv_setup(struct raw_pcb *pcb, mos_sock *sk)
{
	raw_recv(pcb, raw_on_recv, sk);
}
