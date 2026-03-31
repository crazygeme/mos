/*
 * net.c — lwIP network stack initialisation for MOS.
 *
 * Wraps the first registered NIC (eth0) in a lwIP netif, starts DHCP,
 * and drives lwIP's NO_SYS timer machinery via a kernel periodic timer.
 */
#include <net/net.h>
#include <hw/nic.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>

#include <ps/ps.h>

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/dhcp.h>
#include <lwip/ip4_addr.h>
#include <lwip/timeouts.h>
#include <lwip/pbuf.h>
#include <netif/ethernet.h>
#include <int/dsr.h>

/* ── sys_now() required by lwIP in NO_SYS mode ─────────────────────────────── */
u32_t sys_now(void)
{
	return (u32_t)time_now_ms();
}

/* ── RX ring buffer (IRQ → DSR context) ─────────────────────────────────────
 * The NIC ISR must not call malloc/lwIP directly — it may preempt a malloc
 * caller and deadlock.  Instead the ISR enqueues raw frames here (memcpy
 * only, no malloc) and schedules a DSR.  The DSR runs from _task_sched()
 * with interrupts disabled but outside IRQ context, where malloc is safe.
 */
#define NET_RX_RING_SIZE 32
#define NET_RX_MAX_FRAME 1600

typedef struct {
	uint8_t data[NET_RX_MAX_FRAME];
	uint16_t len;
} net_rx_slot_t;

static net_rx_slot_t g_rx_ring[NET_RX_RING_SIZE];
static volatile unsigned g_rx_wr; /* written by IRQ   */
static volatile unsigned g_rx_rd; /* read by DSR      */
static volatile int g_rx_dsr_armed; /* 1 = DSR queued   */

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

/* Called by lwIP to transmit a packet.
 * Uses a static bounce buffer to linearise the pbuf chain — safe because
 * eth0_linkoutput is only called from DSR/task context (no SMP, no reentry). */
static err_t eth0_linkoutput(struct netif *netif, struct pbuf *p)
{
	static uint8_t txbounce[NET_RX_MAX_FRAME];
	nic_dev *nic = (nic_dev *)netif->state;

	if (!nic->send)
		return ERR_IF;

	u16_t len = pbuf_copy_partial(p, txbounce, sizeof(txbounce), 0);
	if (nic->send(nic, txbounce, len) >= 0) {
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

/* DSR: drain queued frames into lwIP (runs from _task_sched, malloc is safe). */
static void eth0_rx_dsr(void *param)
{
	(void)param;
	/* Clear armed flag first so new IRQs that fire during drain will
	 * re-arm and not lose the notification. */
	g_rx_dsr_armed = 0;

	while (g_rx_rd != g_rx_wr) {
		net_rx_slot_t *slot = &g_rx_ring[g_rx_rd % NET_RX_RING_SIZE];
		uint16_t len = slot->len;
		struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
		if (p) {
			pbuf_take(p, slot->data, len);
			g_rx_bytes += len;
			g_rx_packets += 1;
			if (eth0.input(p, &eth0) != ERR_OK)
				pbuf_free(p);
		}
		g_rx_rd++;
	}
}

/* RX enqueue — called by the NIC driver ISR.  Must not call malloc. */
static void eth0_rx_enqueue(void *ctx, const uint8_t *data, uint16_t len)
{
	(void)ctx;
	/* Drop if ring is full. */
	if (g_rx_wr - g_rx_rd >= NET_RX_RING_SIZE)
		return;
	net_rx_slot_t *slot = &g_rx_ring[g_rx_wr % NET_RX_RING_SIZE];
	if (len > NET_RX_MAX_FRAME)
		len = NET_RX_MAX_FRAME;
	memcpy(slot->data, data, len);
	slot->len = len;
	g_rx_wr++;
	if (!g_rx_dsr_armed) {
		g_rx_dsr_armed = 1;
		dsr_add(eth0_rx_dsr, NULL);
	}
}

/* Kernel task: drive all lwIP internal timers every 10 ms (same as tickets). */
static void lwip_timer_task(void *param)
{
	(void)param;
	for (;;) {
		time_wait(10);
		sys_check_timeouts();
		netif_poll_all();
	}
}

/* ── KERNEL_INIT entry point ────────────────────────────────────────────────── */
void net_init(void)
{
	/*
	 * lwip_init() creates the loopback netif (127.0.0.1) unconditionally
	 * inside netif_init().  Do this first so that loopback is always
	 * available, even when no physical NIC is present.
	 */
	lwip_init();

	/* Poll lwIP timers every 100 ms — required for loopback delivery. */
	ps_create(lwip_timer_task, NULL, ps_normal, ps_kernel);

	nic_dev *nic = nic_getdev(0);
	if (!nic) {
		printk("net: no NIC found, loopback only\n");
		return;
	}

	ip4_addr_t ip, mask, gw;
	IP4_ADDR(&ip, 0, 0, 0, 0);
	IP4_ADDR(&mask, 0, 0, 0, 0);
	IP4_ADDR(&gw, 0, 0, 0, 0);

	netif_add(&eth0, &ip, &mask, &gw, nic, eth0_init_fn, ethernet_input);
	netif_set_default(&eth0);
	netif_set_up(&eth0);

	/* Wire NIC → lwIP receive path */
	nic->rx_notify = eth0_rx_enqueue;
	nic->rx_ctx = &eth0;

	dhcp_start(&eth0);

	printk("net: eth0 up, DHCP started\n");

	/* Wait until the interface has a non-zero IP (required for TCP bind). */
	uint32_t deadline = (uint32_t)time_now_ms() + 10000;
	while (ip4_addr_isany(netif_ip4_addr(&eth0)) &&
	       (uint32_t)time_now_ms() < deadline)
		time_wait(10);

	if (!ip4_addr_isany(netif_ip4_addr(&eth0)))
		printk("net: address: %s\n",
		       ip4addr_ntoa(netif_ip4_addr(&eth0)));
	else
		printk("net: address timeout\n");
}

KERNEL_INIT(7, net_init);
