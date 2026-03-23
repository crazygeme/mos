/*
 * net.c — lwIP network stack initialisation for MOS.
 *
 * Wraps the first registered NIC (eth0) in a lwIP netif, starts DHCP,
 * and drives lwIP's NO_SYS timer machinery via a kernel periodic timer.
 */
#include <net/net.h>
#include <hw/nic.h>
#include <lib/klib.h>
#include <lib/timer.h>
#include <hw/time.h>
#include <macro.h>

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/dhcp.h>
#include <lwip/timeouts.h>
#include <lwip/pbuf.h>
#include <netif/ethernet.h>

/* ── sys_now() required by lwIP in NO_SYS mode ─────────────────────────────── */
u32_t sys_now(void)
{
	return (u32_t)time_now_ms();
}

/* ── Single eth0 netif + counters ───────────────────────────────────────────── */
static struct netif eth0;
static unsigned long g_rx_bytes, g_rx_packets;
static unsigned long g_tx_bytes, g_tx_packets;

struct netif *net_get_default_netif(void)
{
	return &eth0;
}

void net_get_stats(net_stats_t *s)
{
	s->rx_bytes = g_rx_bytes;
	s->rx_packets = g_rx_packets;
	s->tx_bytes = g_tx_bytes;
	s->tx_packets = g_tx_packets;
}

/* Called by lwIP to transmit a packet. */
static err_t eth0_linkoutput(struct netif *netif, struct pbuf *p)
{
	nic_dev *nic = (nic_dev *)netif->state;
	uint8_t buf[1600];
	u16_t len = pbuf_copy_partial(p, buf, sizeof(buf), 0);

	if (!nic->send)
		return ERR_IF;
	if (nic->send(nic, buf, (uint16_t)len) >= 0) {
		g_tx_bytes += len;
		g_tx_packets += 1;
		return ERR_OK;
	}
	return ERR_IF;
}

/* lwIP netif init callback — fills in hwaddr, mtu, flags, output functions. */
static err_t eth0_init_fn(struct netif *netif)
{
	nic_dev *nic = (nic_dev *)netif->state;

	netif->hwaddr_len = 6;
	memcpy(netif->hwaddr, nic->mac_addr, 6);
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
		       NETIF_FLAG_LINK_UP;
	netif->name[0] = 'e';
	netif->name[1] = '0';
	netif->output = etharp_output;
	netif->linkoutput = eth0_linkoutput;
	return ERR_OK;
}

/* RX callback — called by the NIC driver ISR with a raw Ethernet frame. */
static void eth0_rx(void *ctx, const uint8_t *data, uint16_t len)
{
	struct netif *netif = (struct netif *)ctx;
	struct pbuf *p;

	p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
	if (!p)
		return;

	g_rx_bytes += len;
	g_rx_packets += 1;

	pbuf_take(p, data, len);
	if (netif->input(p, netif) != ERR_OK)
		pbuf_free(p);
}

/* Periodic timer: drive all lwIP internal timers (DHCP, ARP, TCP, …). */
static void lwip_timer_cb(timer_t *t, void *ctx)
{
	(void)t;
	(void)ctx;
	sys_check_timeouts();
}

/* ── KERNEL_INIT entry point ────────────────────────────────────────────────── */
void net_init(void)
{
	nic_dev *nic = nic_getdev(0);
	if (!nic) {
		printk("net: no NIC found, skipping network init\n");
		return;
	}

	lwip_init();

	ip4_addr_t ip, mask, gw;
	IP4_ADDR(&ip, 0, 0, 0, 0);
	IP4_ADDR(&mask, 0, 0, 0, 0);
	IP4_ADDR(&gw, 0, 0, 0, 0);

	netif_add(&eth0, &ip, &mask, &gw, nic, eth0_init_fn, ethernet_input);
	netif_set_default(&eth0);
	netif_set_up(&eth0);

	/* Wire NIC → lwIP receive path */
	nic->rx_notify = eth0_rx;
	nic->rx_ctx = &eth0;

	dhcp_start(&eth0);

	/* Poll lwIP timers every 100 ms */
	timer_start(lwip_timer_cb, 100, 1, NULL);

	printk("net: eth0 up, DHCP started\n");
}

KERNEL_INIT(7, net_init);
