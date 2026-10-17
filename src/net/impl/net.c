/*
 * net.c — lwIP network stack initialisation for MOS.
 *
 * Wraps the first registered NIC (eth0) in a lwIP netif and starts DHCP.
 */
#include <net/core.h>
#include <ps/namespace.h>
#include <net/net.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <macro.h>

#include <ps/ps.h>

#include <lwip/init.h>
#include <lwip/ip.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/dhcp.h>
#include <lwip/ip4_addr.h>
#include <lwip/timeouts.h>
#include <lwip/pbuf.h>
#include <netif/ethernet.h>
#include <int/dsr.h>
#include <int/int.h>

/* ── sys_now() required by lwIP in NO_SYS mode ─────────────────────────────── */
u32_t sys_now(void)
{
	return (u32_t)time_now_ms();
}

void net_set_interface_up(struct netif *nif, int up)
{
	if (up) {
		netif_set_up(nif);
	} else {
		dhcp_stop(nif);
		netif_set_down(nif);
	}
}

static mutex_t net_core_mutex;
static spinlock_t net_service_lock;

int net_core_enter(void)
{
	if (!ps_enabled())
		return 0;
	sched_disable();
	if (!current->sched->net_core_depth)
		mutex_lock(&net_core_mutex);
	current->sched->net_core_depth++;
	return 1;
}

void net_core_unlock(void)
{
	if (!--current->sched->net_core_depth)
		mutex_unlock(&net_core_mutex);
	sched_enable();
}

static int net_guard_enter(void *context __attribute__((unused)))
{
	return net_core_enter();
}

static void net_local_guard_leave(void *context __attribute__((unused)),
				  int active)
{
	if (active)
		net_core_unlock();
}

static void net_core_guard_leave(void *context, int active)
{
	net_service_update();
	net_local_guard_leave(context, active);
}

static const lock_operations_t net_core_operations = { net_guard_enter,
						       net_core_guard_leave };
static const lock_operations_t net_local_operations = { net_guard_enter,
							net_local_guard_leave };

const scoped_lock_t net_core_lock = { .header = { &net_core_operations } };
const scoped_lock_t net_local_lock = { .header = { &net_local_operations } };

unsigned net_core_suspend(void)
{
	unsigned depth = current->sched->net_core_depth;
	if (depth) {
		current->sched->net_core_depth = 0;
		mutex_unlock(&net_core_mutex);
	}
	return depth;
}

void net_core_resume(unsigned depth)
{
	if (depth) {
		mutex_lock(&net_core_mutex);
		current->sched->net_core_depth = depth;
	}
}

static int net_service_initialized;
static int net_service_queued;
static unsigned long long net_service_due = ~0ULL;

static void net_service_run(void *param)
{
	(void)param;
	LOCK_GUARD(&net_core_lock);
	int irq;
	spinlock_lock(&net_service_lock, &irq);
	net_service_queued = 0;
	spinlock_unlock(&net_service_lock, irq);
	sys_check_timeouts();
	netif_poll_all();
}

/* Requires net_service_lock; interrupt context only publishes deferred work. */
static void net_service_queue(void)
{
	if (!net_service_queued) {
		net_service_queued = 1;
		if (!dsr_add(net_service_run, NULL)) {
			net_service_queued = 0;
			net_service_due = time_now_ms() + TICK_MS;
		}
	}
}

/* Refresh the deadline after every stack operation, including new TCP timers.
 * Loopback packets enter deferred work immediately rather than waiting for PIT. */
void net_service_update(void)
{
	struct netif *nif;
	u32_t delay;
	unsigned long long due;
	int irq, loopback = 0;

	if (!net_service_initialized)
		return;
	delay = sys_timeouts_sleeptime();
	due = delay == SYS_TIMEOUTS_SLEEPTIME_INFINITE ? ~0ULL :
							 time_now_ms() + delay;
	NETIF_FOREACH(nif)
	{
		if (nif->loop_first) {
			loopback = 1;
			break;
		}
	}
	spinlock_lock(&net_service_lock, &irq);
	net_service_due = due;
	if (loopback || !delay)
		net_service_queue();
	spinlock_unlock(&net_service_lock, irq);
}

/* IRQ context only queues due work. lwIP callbacks run on the DSR worker. */
void net_service_tick(void)
{
	int irq;
	spinlock_lock(&net_service_lock, &irq);
	if (net_service_initialized && time_coarse_ms() >= net_service_due)
		net_service_queue();
	spinlock_unlock(&net_service_lock, irq);
}

/* IRQ context queues DMA references without allocation. Deferred work copies
 * frames into the lwIP pool and returns DMA descriptors before stack input,
 * so TCP reassembly and receive backpressure cannot exhaust the NIC ring. */
#define NET_RX_RING_SIZE 256
#define NET_RX_MAX_FRAME 1600

typedef struct {
	nic_dev *nic;
	const uint8_t *data;
	uint16_t len;
	void *cookie;
	list_entry free_list;
} net_rx_slot_t;

static net_rx_slot_t g_rx_slots[NET_RX_RING_SIZE];
static uint16_t g_rx_queue[NET_RX_RING_SIZE];
static volatile unsigned g_rx_wr; /* written by IRQ   */
static volatile unsigned g_rx_rd; /* read by DSR      */
static volatile int g_rx_dsr_armed; /* 1 = DSR queued   */
static list_entry g_rx_free_list;
static spinlock_t g_rx_lock;

/* ── Single eth0 netif + counters ───────────────────────────────────────────── */
static struct netif eth0;
static unsigned long g_rx_bytes, g_rx_packets;
static unsigned long g_tx_bytes, g_tx_packets;

struct netif *net_get_default_netif(void)
{
	return net_namespace_current() ? NULL : &eth0;
}

unsigned net_namespace_current(void)
{
	void *ns = ps_netns_get(current);
	unsigned tag = ps_netns_tag(ns);
	ps_netns_put(ns);
	return tag;
}
int net_namespace_visible(const struct netif *nif)
{
	return nif->mos_netns == net_namespace_current();
}
static err_t namespace_loop_output(struct netif *nif, struct pbuf *p,
				   const ip4_addr_t *dest)
{
	if (!ip4_addr_isloopback(dest) || !netif_is_up(nif))
		return ERR_RTE;
	return netif_loop_output(nif, p);
}
static err_t namespace_loop_init(struct netif *nif)
{
	nif->name[0] = 'l';
	nif->name[1] = 'o';
	nif->mtu = 65535;
	nif->output = namespace_loop_output;
	return ERR_OK;
}
struct netif *net_namespace_create(unsigned tag)
{
	LOCK_GUARD(&net_core_lock);
	unsigned count = 0;
	for (struct netif *n = netif_list; n; n = n->next)
		count++;
	if (count >= 254)
		return NULL;
	struct netif *nif = zalloc(sizeof(*nif));
	if (!nif)
		return NULL;
	ip4_addr_t ip, mask, gw;
	IP4_ADDR(&ip, 127, 0, 0, 1);
	IP4_ADDR(&mask, 255, 0, 0, 0);
	IP4_ADDR(&gw, 0, 0, 0, 0);
	if (!netif_add(nif, &ip, &mask, &gw, NULL, namespace_loop_init,
		       ip_input)) {
		free(nif);
		return NULL;
	}
	nif->mos_netns = tag;
	netif_set_link_up(nif);
	return nif;
}
void net_namespace_destroy(struct netif *nif)
{
	LOCK_GUARD(&net_core_lock);
	netif_set_down(nif);
	for (unsigned i = 0; i < NUM_TCP_PCB_LISTS; i++) {
		struct tcp_pcb *p = *tcp_pcb_lists[i];
		while (p) {
			struct tcp_pcb *next = p->next;
			if (p->mos_netns == nif->mos_netns) {
				if (p->state == LISTEN)
					tcp_close(p);
				else
					tcp_abort(p);
			}
			p = next;
		}
	}
	netif_poll(nif);
	netif_remove(nif);
	free(nif);
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
 * callers hold the network-core mutex and suppress task preemption. */
static err_t eth0_linkoutput(struct netif *netif, struct pbuf *p)
{
	static uint8_t txbounce[NET_RX_MAX_FRAME];
	nic_dev *nic = (nic_dev *)netif->state;
	u16_t len;

	if (!nic->ops || (!nic->ops->send && !nic->ops->send_pbuf))
		return ERR_IF;

	if (nic->ops->send_pbuf) {
		len = p->tot_len;
		if (nic->ops->send_pbuf(nic, p) >= 0) {
			g_tx_bytes += len;
			g_tx_packets += 1;
			return ERR_OK;
		}
		return ERR_IF;
	}

	len = pbuf_copy_partial(p, txbounce, sizeof(txbounce), 0);
	if (nic->ops->send(nic, txbounce, len) >= 0) {
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

	if (slot->nic && slot->nic->ops && slot->nic->ops->rx_reclaim)
		slot->nic->ops->rx_reclaim(slot->nic, slot->cookie);

	slot->nic = NULL;
	slot->data = NULL;
	slot->len = 0;
	slot->cookie = NULL;

	spinlock_lock(&g_rx_lock, &irq);
	list_insert_head(&g_rx_free_list, &slot->free_list);
	spinlock_unlock(&g_rx_lock, irq);
}

/* Drain queued frames into lwIP on the deferred-callback worker. */
static void eth0_rx_dsr(void *param)
{
	(void)param;
	/* Clear armed flag first so new IRQs that fire during drain will
	 * re-arm and not lose the notification. */
	int armed_irq;
	spinlock_lock(&g_rx_lock, &armed_irq);
	g_rx_dsr_armed = 0;
	spinlock_unlock(&g_rx_lock, armed_irq);

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

		LOCK_GUARD(&net_core_lock);
		slot = &g_rx_slots[idx];
		p = pbuf_alloc(PBUF_RAW, slot->len, PBUF_POOL);
		if (p && pbuf_take(p, slot->data, slot->len) != ERR_OK) {
			pbuf_free(p);
			p = NULL;
		}
		if (p) {
			g_rx_bytes += slot->len;
			g_rx_packets += 1;
		}
		eth0_rx_release_slot(slot);
		if (!p)
			continue;

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
	if (g_rx_wr - g_rx_rd >= NET_RX_RING_SIZE ||
	    list_is_empty(&g_rx_free_list)) {
		spinlock_unlock(&g_rx_lock, irq);
		return 0;
	}

	net_rx_slot_t *slot = container_of(list_remove_head(&g_rx_free_list),
					   net_rx_slot_t, free_list);
	idx = slot - g_rx_slots;
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
			list_insert_head(&g_rx_free_list, &slot->free_list);
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
	mutex_init(&net_core_mutex);
	spinlock_init(&net_service_lock);
	LOCK_GUARD(&net_core_lock);
	/*
	 * lwip_init() creates the loopback netif (127.0.0.1) unconditionally
	 * inside netif_init().  Do this first so that loopback is always
	 * available, even when no physical NIC is present.
	 */
	lwip_init();
	net_service_initialized = 1;
	spinlock_init(&g_rx_lock);
	g_rx_wr = 0;
	g_rx_rd = 0;
	g_rx_dsr_armed = 0;
	list_init(&g_rx_free_list);
	for (int i = 0; i < NET_RX_RING_SIZE; i++) {
		g_rx_slots[i].nic = NULL;
		g_rx_slots[i].data = NULL;
		g_rx_slots[i].len = 0;
		g_rx_slots[i].cookie = NULL;
		list_insert_tail(&g_rx_free_list, &g_rx_slots[i].free_list);
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
