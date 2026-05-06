/*
 * net.c — lwIP network stack initialisation for MOS.
 *
 * Wraps the first registered NIC (eth0) in a lwIP netif and starts DHCP.
 */
#include <net/net.h>
#include <hw/nic.h>
#include <lib/klib.h>
#include <lib/lock.h>
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
	return (u32_t)(time_now_us() / 1000);
}

/* ── RX ring buffer (IRQ → DSR context) ─────────────────────────────────────
 * The NIC ISR must not call malloc/lwIP directly — it may preempt a malloc
 * caller and deadlock. Instead the ISR enqueues references to NIC DMA
 * buffers here and schedules a DSR. The DSR wraps each DMA buffer in a
 * custom PBUF_REF so lwIP can parse it without an extra memcpy, and the
 * descriptor is only returned to hardware once lwIP frees the pbuf.
 */
#define NET_RX_RING_SIZE 256
#define NET_RX_MAX_FRAME 1600

typedef struct {
	struct pbuf_custom custom;
	nic_dev *nic;
	const uint8_t *data;
	uint16_t len;
	void *cookie;
	int next_free;
} net_rx_slot_t;

static net_rx_slot_t g_rx_slots[NET_RX_RING_SIZE];
static uint16_t g_rx_queue[NET_RX_RING_SIZE];
static volatile unsigned g_rx_wr; /* written by IRQ   */
static volatile unsigned g_rx_rd; /* read by DSR      */
static volatile int g_rx_dsr_armed; /* 1 = DSR queued   */
static int g_rx_free_head = -1;
static spinlock_t g_rx_lock;

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
	u16_t len;

	if (!nic->send && !nic->send_pbuf)
		return ERR_IF;

	if (nic->send_pbuf) {
		len = p->tot_len;
		if (nic->send_pbuf(nic, p) >= 0) {
			g_tx_bytes += len;
			g_tx_packets += 1;
			return ERR_OK;
		}
		return ERR_IF;
	}

	len = pbuf_copy_partial(p, txbounce, sizeof(txbounce), 0);
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

static void eth0_rx_release_slot(net_rx_slot_t *slot)
{
	int irq;
	int idx = (int)(slot - g_rx_slots);

	if (slot->nic && slot->nic->rx_reclaim)
		slot->nic->rx_reclaim(slot->nic, slot->cookie);

	slot->nic = NULL;
	slot->data = NULL;
	slot->len = 0;
	slot->cookie = NULL;

	spinlock_lock(&g_rx_lock, &irq);
	slot->next_free = g_rx_free_head;
	g_rx_free_head = idx;
	spinlock_unlock(&g_rx_lock, irq);
}

static void eth0_rx_free_pbuf(struct pbuf *p)
{
	net_rx_slot_t *slot =
		container_of((struct pbuf_custom *)p, net_rx_slot_t, custom);
	eth0_rx_release_slot(slot);
}

/* DSR: drain queued frames into lwIP (runs from _task_sched, malloc is safe). */
static void eth0_rx_dsr(void *param)
{
	(void)param;
	/* Clear armed flag first so new IRQs that fire during drain will
	 * re-arm and not lose the notification. */
	g_rx_dsr_armed = 0;

	while (g_rx_rd != g_rx_wr) {
		int irq;
		unsigned idx;
		net_rx_slot_t *slot;
		struct pbuf *p;

		spinlock_lock(&g_rx_lock, &irq);
		if (g_rx_rd == g_rx_wr) {
			spinlock_unlock(&g_rx_lock, irq);
			break;
		}
		idx = g_rx_queue[g_rx_rd % NET_RX_RING_SIZE];
		g_rx_rd++;
		spinlock_unlock(&g_rx_lock, irq);

		slot = &g_rx_slots[idx];
		slot->custom.custom_free_function = eth0_rx_free_pbuf;
		p = pbuf_alloced_custom(PBUF_RAW, slot->len, PBUF_REF,
					&slot->custom, (void *)slot->data,
					slot->len);
		if (!p) {
			eth0_rx_release_slot(slot);
			continue;
		}

		g_rx_bytes += slot->len;
		g_rx_packets += 1;
		if (eth0.input(p, &eth0) != ERR_OK)
			pbuf_free(p);
	}
}

/* RX enqueue — called by the NIC driver ISR.  Must not call malloc. */
static int eth0_rx_enqueue(void *ctx, const uint8_t *data, uint16_t len,
			   void *cookie)
{
	int irq;
	int idx;

	(void)ctx;
	if (len > NET_RX_MAX_FRAME)
		return 0;

	spinlock_lock(&g_rx_lock, &irq);
	/* Drop if the queue is full or there are no free tracking slots. */
	if (g_rx_wr - g_rx_rd >= NET_RX_RING_SIZE || g_rx_free_head < 0) {
		spinlock_unlock(&g_rx_lock, irq);
		return 0;
	}

	idx = g_rx_free_head;
	g_rx_free_head = g_rx_slots[idx].next_free;
	net_rx_slot_t *slot = &g_rx_slots[idx];
	slot->nic = (nic_dev *)eth0.state;
	slot->data = data;
	slot->len = len;
	slot->cookie = cookie;
	g_rx_queue[g_rx_wr % NET_RX_RING_SIZE] = (uint16_t)idx;
	g_rx_wr++;
	if (!g_rx_dsr_armed) {
		g_rx_dsr_armed = 1;
		if (!dsr_add(eth0_rx_dsr, NULL)) {
			g_rx_dsr_armed = 0;
			g_rx_wr--;
			g_rx_free_head = idx;
			spinlock_unlock(&g_rx_lock, irq);
			return 0;
		}
	}
	spinlock_unlock(&g_rx_lock, irq);
	return 1;
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
	spinlock_init(&g_rx_lock);
	g_rx_wr = 0;
	g_rx_rd = 0;
	g_rx_dsr_armed = 0;
	g_rx_free_head = 0;
	for (int i = 0; i < NET_RX_RING_SIZE; i++) {
		g_rx_slots[i].nic = NULL;
		g_rx_slots[i].data = NULL;
		g_rx_slots[i].len = 0;
		g_rx_slots[i].cookie = NULL;
		g_rx_slots[i].next_free = (i + 1 < NET_RX_RING_SIZE) ? (i + 1) :
								       -1;
	}

	/* Start shared periodic services after lwIP is initialized. */
	ps_start_system_services();

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

	if (0) {
		/* Wait until the interface has a non-zero IP (required for TCP bind). */
		uint64_t deadline = time_now_ms() + 10000;
		while (ip4_addr_isany(netif_ip4_addr(&eth0)) &&
		       time_now_ms() < deadline)
			time_wait(10);

		if (!ip4_addr_isany(netif_ip4_addr(&eth0)))
			printk("net: address: %s\n",
			       ip4addr_ntoa(netif_ip4_addr(&eth0)));
		else
			printk("net: address timeout\n");
	}
}

KERNEL_INIT(7, net_init);
