/*
 * Intel 8254x (e1000) NIC driver.
 * Supports 82540EM (QEMU default "e1000", PCI 8086:100E).
 *
 * Uses legacy descriptor format, polled TX, interrupt-driven RX.
 * MMIO is at BAR0 (memory-mapped, >= 0xC0000000 in QEMU → mapped with
 * mm_map_io).  DMA buffers live in kernel heap; PA is resolved from the PTE.
 */
#include <hw/nic.h>
#include <hw/pci.h>
#include <hw/driver.h>
#include <lib/klib.h>
#include <lib/port.h>
#include <mm/mm.h>
#include <int/int.h>
#include <macro.h>
#include <config.h>
#include <mm/phymm.h>
#include <stdint.h>
#include <lwip/pbuf.h>

/* ── MMIO register offsets ─────────────────────────────────────────────────── */
#define E1000_CTRL 0x0000
#define E1000_STATUS 0x0008
#define E1000_EERD 0x0014
#define E1000_ICR 0x00C0
#define E1000_IMS 0x00D0
#define E1000_IMC 0x00D8
#define E1000_RCTL 0x0100
#define E1000_TCTL 0x0400
#define E1000_TIPG 0x0410
#define E1000_RDBAL 0x2800
#define E1000_RDBAH 0x2804
#define E1000_RDLEN 0x2808
#define E1000_RDH 0x2810
#define E1000_RDT 0x2818
#define E1000_TDBAL 0x3800
#define E1000_TDBAH 0x3804
#define E1000_TDLEN 0x3808
#define E1000_TDH 0x3810
#define E1000_TDT 0x3818
#define E1000_MTA_BASE 0x5200
#define E1000_RAL0 0x5400
#define E1000_RAH0 0x5404

/* ── Control / status bits ─────────────────────────────────────────────────── */
#define E1000_CTRL_SLU (1u << 6) /* Set Link Up */
#define E1000_CTRL_RST (1u << 26) /* Full device reset */

#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_SBP (1u << 2)
#define E1000_RCTL_UPE (1u << 3) /* unicast promiscuous */
#define E1000_RCTL_MPE (1u << 4) /* multicast promiscuous */
#define E1000_RCTL_BAM (1u << 15) /* accept broadcast */
#define E1000_RCTL_SECRC (1u << 26) /* strip CRC */
/* BSIZE=2048: BSIZE bits [17:16]=00, BSEX bit[25]=0 — these are 0 by default */

#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3) /* pad short packets */

/* ICR bits */
#define E1000_ICR_TXDW (1u << 0) /* TX descriptor written back */
#define E1000_ICR_RXT0 (1u << 7) /* RX timer / packet received */
#define E1000_ICR_LSC (1u << 2) /* link status change */

/* RX descriptor status */
#define E1000_RXD_STAT_DD 0x01u /* descriptor done */
#define E1000_RXD_STAT_EOP 0x02u /* end of packet */

/* TX descriptor command */
#define E1000_TXD_CMD_EOP 0x01u
#define E1000_TXD_CMD_IFCS 0x02u /* insert FCS/CRC */
#define E1000_TXD_CMD_RS 0x08u /* report status */

/* TX descriptor status */
#define E1000_TXD_STAT_DD 0x01u /* descriptor done */

/* ── Descriptor ring sizes ─────────────────────────────────────────────────── */
#define E1000_NUM_RX_DESC 256
#define E1000_NUM_TX_DESC 64
#define E1000_RX_BUF_SIZE 2048
#define E1000_TX_BUF_SIZE 2048

/* ── Descriptor layouts (legacy) ───────────────────────────────────────────── */
struct e1000_rx_desc {
	uint64_t addr; /* physical address of receive buffer */
	uint16_t length;
	uint16_t checksum;
	uint8_t status;
	uint8_t errors;
	uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
	uint64_t addr; /* physical address of transmit buffer */
	uint16_t length;
	uint8_t cso;
	uint8_t cmd;
	uint8_t status;
	uint8_t css;
	uint16_t special;
} __attribute__((packed));

/* ── Per-device context ─────────────────────────────────────────────────────── */
typedef struct {
	uint32_t mmio_base; /* virtual (== physical) MMIO base */
	uint8_t irq_line;

	struct e1000_rx_desc *rx_descs;
	uint32_t rx_bufs_va; /* base VA; buf i at rx_bufs_va + i*RX_BUF_SIZE */
	uint16_t rx_cur;
	uint16_t rx_reclaim_head;
	uint8_t rx_reclaim_ready[E1000_NUM_RX_DESC];

	struct e1000_tx_desc *tx_descs;
	uint32_t tx_bufs_va;
	uint16_t tx_cur;

	nic_dev *nic; /* pointer to the permanent nic_dev in network_devices[] */
} e1000_ctx;

/* Single global device (one e1000 expected) — used by the IRQ handler. */
static e1000_ctx *g_e1000_ctx;

/* ── MMIO accessors ─────────────────────────────────────────────────────────── */
static inline uint32_t e1000_rd(e1000_ctx *ctx, uint32_t off)
{
	return *(volatile uint32_t *)(ctx->mmio_base + off);
}

static inline void e1000_wr(e1000_ctx *ctx, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(ctx->mmio_base + off) = val;
}

/* ── EEPROM read (82540EM "old" EERD format) ────────────────────────────────── */
static uint16_t e1000_eeprom_read(e1000_ctx *ctx, uint8_t addr)
{
	uint32_t v;
	e1000_wr(ctx, E1000_EERD, ((uint32_t)addr << 8) | 1u);
	do {
		v = e1000_rd(ctx, E1000_EERD);
	} while (!(v & (1u << 4))); /* DONE bit */
	return (uint16_t)(v >> 16);
}

/* ── Map BAR0 MMIO pages ────────────────────────────────────────────────────── */
static uint32_t e1000_map_mmio(uint32_t bar0)
{
	uint32_t phys = bar0 & ~0xFu; /* strip BAR flags */
	unsigned i;
	/* 82540EM BAR0 = 128 KB = 32 pages */
	for (i = 0; i < 32; i++)
		mm_map_io(phys + i * PAGE_SIZE);
	return phys;
}

/* ── RX handling (called from IRQ) ─────────────────────────────────────────── */
static void e1000_handle_rx(e1000_ctx *ctx)
{
	while (1) {
		uint16_t idx = ctx->rx_cur;
		struct e1000_rx_desc *desc = &ctx->rx_descs[idx];
		if (!(desc->status & E1000_RXD_STAT_DD))
			break;

		uint16_t len = desc->length;
		uint8_t *buf = (uint8_t *)(ctx->rx_bufs_va +
					   (uint32_t)idx * E1000_RX_BUF_SIZE);

		int queued = 0;
		if (len > 0 && ctx->nic && ctx->nic->rx_notify)
			queued = ctx->nic->rx_notify(ctx->nic->rx_ctx, buf, len,
						     (void *)(uintptr_t)idx);

		if (!queued) {
			ctx->rx_reclaim_ready[idx] = 1;
			while (ctx->rx_reclaim_ready[ctx->rx_reclaim_head]) {
				uint16_t reclaim = ctx->rx_reclaim_head;
				ctx->rx_reclaim_ready[reclaim] = 0;
				ctx->rx_descs[reclaim].status = 0;
				e1000_wr(ctx, E1000_RDT, reclaim);
				ctx->rx_reclaim_head =
					(uint16_t)((reclaim + 1) %
						   E1000_NUM_RX_DESC);
			}
		}

		ctx->rx_cur = (uint16_t)((ctx->rx_cur + 1) % E1000_NUM_RX_DESC);
	}
}

static void nic_intel_8254x_rx_reclaim(void *_dev, void *cookie)
{
	nic_dev *dev = (nic_dev *)_dev;
	e1000_ctx *ctx = (e1000_ctx *)dev->ctx;
	uint16_t idx = (uint16_t)(uintptr_t)cookie;

	if (idx >= E1000_NUM_RX_DESC)
		return;

	ctx->rx_reclaim_ready[idx] = 1;
	while (ctx->rx_reclaim_ready[ctx->rx_reclaim_head]) {
		uint16_t reclaim = ctx->rx_reclaim_head;
		ctx->rx_reclaim_ready[reclaim] = 0;
		ctx->rx_descs[reclaim].status = 0;
		e1000_wr(ctx, E1000_RDT, reclaim);
		ctx->rx_reclaim_head =
			(uint16_t)((reclaim + 1) % E1000_NUM_RX_DESC);
	}
}

/* ── Interrupt handler ──────────────────────────────────────────────────────── */
static void e1000_irq_handler(intr_frame *frame)
{
	(void)frame;
	if (!g_e1000_ctx)
		return;

	uint32_t icr = e1000_rd(g_e1000_ctx, E1000_ICR);
	if (icr & (E1000_ICR_RXT0 | E1000_ICR_TXDW))
		e1000_handle_rx(g_e1000_ctx);
}

/* ── Transmit ────────────────────────────────────────────────────────────────── */
static int nic_intel_8254x_send(void *_dev, const void *buf, uint16_t len)
{
	nic_dev *dev = (nic_dev *)_dev;
	e1000_ctx *ctx = (e1000_ctx *)dev->ctx;
	uint16_t tail = ctx->tx_cur;
	struct e1000_tx_desc *desc = &ctx->tx_descs[tail];

	/* wait for previous descriptor to be reclaimed */
	while (!(desc->status & E1000_TXD_STAT_DD))
		HLT();

	uint8_t *txbuf = (uint8_t *)(ctx->tx_bufs_va +
				     (uint32_t)tail * E1000_TX_BUF_SIZE);
	if (len > E1000_TX_BUF_SIZE)
		len = (uint16_t)E1000_TX_BUF_SIZE;
	memcpy(txbuf, buf, len);

	desc->addr = (uint64_t)VIRT_TO_PHY(txbuf);
	desc->length = len;
	desc->cso = 0;
	desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
	desc->status = 0;
	desc->css = 0;
	desc->special = 0;

	ctx->tx_cur = (uint16_t)((tail + 1) % E1000_NUM_TX_DESC);
	e1000_wr(ctx, E1000_TDT, ctx->tx_cur);
	return len;
}

static int nic_intel_8254x_send_pbuf(void *_dev, const struct pbuf *p)
{
	nic_dev *dev = (nic_dev *)_dev;
	e1000_ctx *ctx = (e1000_ctx *)dev->ctx;
	uint16_t tail = ctx->tx_cur;
	struct e1000_tx_desc *desc = &ctx->tx_descs[tail];
	uint8_t *txbuf;
	uint16_t len;
	uint16_t off = 0;
	const struct pbuf *q;

	if (!p || p->tot_len > E1000_TX_BUF_SIZE)
		return -1;

	/* wait for previous descriptor to be reclaimed */
	while (!(desc->status & E1000_TXD_STAT_DD))
		HLT();

	txbuf = (uint8_t *)(ctx->tx_bufs_va +
			    (uint32_t)tail * E1000_TX_BUF_SIZE);
	len = p->tot_len;
	for (q = p; q && off < len; q = q->next) {
		uint16_t copy = q->len;
		if (copy > len - off)
			copy = (uint16_t)(len - off);
		memcpy(txbuf + off, q->payload, copy);
		off = (uint16_t)(off + copy);
	}
	if (off != len)
		return -1;

	desc->addr = (uint64_t)VIRT_TO_PHY(txbuf);
	desc->length = len;
	desc->cso = 0;
	desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
	desc->status = 0;
	desc->css = 0;
	desc->special = 0;

	ctx->tx_cur = (uint16_t)((tail + 1) % E1000_NUM_TX_DESC);
	e1000_wr(ctx, E1000_TDT, ctx->tx_cur);
	return len;
}

/* ── init / uninit ───────────────────────────────────────────────────────────── */
static int nic_intel_8254x_init(void *_dev)
{
	nic_dev *dev = (nic_dev *)_dev;
	unsigned i;

	/* ── Read BAR0 ── */
	uint32_t bar0 = pci_read_field(dev->pci_dev, PCI_BAR0, 4);
	if (bar0 & 1) {
		printk("net: BAR0 is I/O space — not supported\n");
		return -1;
	}

	/* ── Map MMIO ── */
	e1000_ctx *ctx = (e1000_ctx *)zalloc(sizeof(*ctx));
	ctx->mmio_base = e1000_map_mmio(bar0);
	ctx->nic =
		NULL; /* patched by nic_intel_8254x_set_nic() after register */
	dev->ctx = ctx;

	/* ── Enable PCI bus master + memory access ── */
	uint32_t cmd = pci_read_field(dev->pci_dev, PCI_COMMAND, 2);
	/* write back with bus-master (bit2) and mem-space (bit1) enabled */
	pci_write_field(dev->pci_dev, PCI_COMMAND, 2, cmd | 0x06);

	/* ── Reset ── */
	e1000_wr(ctx, E1000_CTRL, e1000_rd(ctx, E1000_CTRL) | E1000_CTRL_RST);
	/* spin until reset clears */
	while (e1000_rd(ctx, E1000_CTRL) & E1000_CTRL_RST)
		HLT();

	/* ── Set Link Up ── */
	e1000_wr(ctx, E1000_CTRL, e1000_rd(ctx, E1000_CTRL) | E1000_CTRL_SLU);

	/* ── Read MAC from EEPROM ── */
	for (i = 0; i < 3; i++) {
		uint16_t w = e1000_eeprom_read(ctx, (uint8_t)i);
		dev->mac_addr[i * 2] = (uint8_t)(w & 0xFF);
		dev->mac_addr[i * 2 + 1] = (uint8_t)(w >> 8);
	}
	printk("net: MAC %02x:%02x:%02x:%02x:%02x:%02x\n", dev->mac_addr[0],
	       dev->mac_addr[1], dev->mac_addr[2], dev->mac_addr[3],
	       dev->mac_addr[4], dev->mac_addr[5]);

	/* ── Multicast table: clear ── */
	for (i = 0; i < 128; i++)
		e1000_wr(ctx, E1000_MTA_BASE + i * 4, 0);

	/* ── RX descriptor ring ── */
	unsigned rx_desc_pages =
		(sizeof(struct e1000_rx_desc) * E1000_NUM_RX_DESC + PAGE_SIZE -
		 1) /
		PAGE_SIZE;
	ctx->rx_descs = (struct e1000_rx_desc *)vm_alloc(rx_desc_pages);
	memset(ctx->rx_descs, 0,
	       sizeof(struct e1000_rx_desc) * E1000_NUM_RX_DESC);

	unsigned rx_buf_pages =
		(E1000_RX_BUF_SIZE * E1000_NUM_RX_DESC + PAGE_SIZE - 1) /
		PAGE_SIZE;
	ctx->rx_bufs_va = vm_alloc(rx_buf_pages);
	ctx->rx_cur = 0;
	ctx->rx_reclaim_head = 0;
	memset(ctx->rx_reclaim_ready, 0, sizeof(ctx->rx_reclaim_ready));

	for (i = 0; i < E1000_NUM_RX_DESC; i++) {
		uint32_t va = ctx->rx_bufs_va + (uint32_t)i * E1000_RX_BUF_SIZE;
		ctx->rx_descs[i].addr = (uint64_t)VIRT_TO_PHY(va);
	}

	uint32_t rx_phys = VIRT_TO_PHY(ctx->rx_descs);
	e1000_wr(ctx, E1000_RDBAL, rx_phys);
	e1000_wr(ctx, E1000_RDBAH, 0);
	e1000_wr(ctx, E1000_RDLEN,
		 E1000_NUM_RX_DESC * (uint32_t)sizeof(struct e1000_rx_desc));
	e1000_wr(ctx, E1000_RDH, 0);
	e1000_wr(ctx, E1000_RDT, E1000_NUM_RX_DESC - 1);

	e1000_wr(ctx, E1000_RCTL,
		 E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE |
			 E1000_RCTL_MPE | E1000_RCTL_BAM | E1000_RCTL_SECRC);

	/* ── TX descriptor ring ── */
	unsigned tx_desc_pages =
		(sizeof(struct e1000_tx_desc) * E1000_NUM_TX_DESC + PAGE_SIZE -
		 1) /
		PAGE_SIZE;
	ctx->tx_descs = (struct e1000_tx_desc *)vm_alloc(tx_desc_pages);
	memset(ctx->tx_descs, 0,
	       sizeof(struct e1000_tx_desc) * E1000_NUM_TX_DESC);

	unsigned tx_buf_pages =
		(E1000_TX_BUF_SIZE * E1000_NUM_TX_DESC + PAGE_SIZE - 1) /
		PAGE_SIZE;
	ctx->tx_bufs_va = vm_alloc(tx_buf_pages);
	ctx->tx_cur = 0;

	/* Pre-mark all TX descriptors as done so the first send doesn't stall. */
	for (i = 0; i < E1000_NUM_TX_DESC; i++)
		ctx->tx_descs[i].status = E1000_TXD_STAT_DD;

	uint32_t tx_phys = VIRT_TO_PHY(ctx->tx_descs);
	e1000_wr(ctx, E1000_TDBAL, tx_phys);
	e1000_wr(ctx, E1000_TDBAH, 0);
	e1000_wr(ctx, E1000_TDLEN,
		 E1000_NUM_TX_DESC * (uint32_t)sizeof(struct e1000_tx_desc));
	e1000_wr(ctx, E1000_TDH, 0);
	e1000_wr(ctx, E1000_TDT, 0);

	/* Inter-packet gap (82540EM recommended: 0x00602006) */
	e1000_wr(ctx, E1000_TIPG, 0x00602006u);

	e1000_wr(ctx, E1000_TCTL,
		 E1000_TCTL_EN | E1000_TCTL_PSP |
			 (0x10u << 4) | /* collision threshold */
			 (0x40u << 12)); /* collision distance */

	/* ── IRQ ── */
	ctx->irq_line =
		(uint8_t)pci_read_field(dev->pci_dev, PCI_INTERRUPT_LINE, 1);
	int vec = INT_VECTOR_IRQ0 + ctx->irq_line;
	int_register(vec, e1000_irq_handler, 0, 0);

	/* Enable RX interrupt */
	e1000_wr(ctx, E1000_IMS, E1000_ICR_RXT0 | E1000_ICR_LSC);

	g_e1000_ctx = ctx;
	printk("net: initialized (BAR0=0x%x IRQ=%d)\n", ctx->mmio_base,
	       ctx->irq_line);
	return 0;
}

static void nic_intel_8254x_on_register(nic_dev *permanent)
{
	e1000_ctx *ctx = (e1000_ctx *)permanent->ctx;
	if (ctx)
		ctx->nic = permanent;
}

static const nic_ops nic_intel_8254x_ops = {
	.init = nic_intel_8254x_init,
	.on_register = nic_intel_8254x_on_register,
	.send = nic_intel_8254x_send,
	.send_pbuf = nic_intel_8254x_send_pbuf,
	.rx_reclaim = nic_intel_8254x_rx_reclaim,
};

static nic_dev *nic_intel_8254x_create(uint32_t device, uint16_t v, uint16_t d)
{
	nic_dev *dev = (nic_dev *)zalloc(sizeof(*dev));
	dev->pci_dev = device;
	dev->ven = v;
	dev->dev = d;
	dev->ops = &nic_intel_8254x_ops;
	return dev;
}

static int nic_intel_8254x_probe_pci(uint32_t device, uint16_t v, uint16_t d,
				     const hw_pci_id *id)
{
	nic_dev *dev;
	nic_dev *registered;

	(void)id;
	dev = nic_intel_8254x_create(device, v, d);
	if (!dev)
		return -1;

	registered = nic_register_device(dev);
	free(dev);
	return registered ? 0 : -1;
}

static const hw_pci_id nic_intel_8254x_ids[] = {
	{ 0x8086, 0x100E },
	{ 0x8086, 0x100F },
	{ 0x8086, 0x1000 },
	{ 0x8086, 0x1001 },
};

static hw_driver nic_intel_8254x_driver = {
	.name = "intel-8254x",
	.type = HW_TYPE_NET,
	.bus = HW_BUS_PCI,
	.pci_ids = nic_intel_8254x_ids,
	.pci_id_count =
		sizeof(nic_intel_8254x_ids) / sizeof(nic_intel_8254x_ids[0]),
	.ops = &nic_intel_8254x_ops,
	.probe_pci = nic_intel_8254x_probe_pci,
};

static void nic_intel_8254x_register(void)
{
	hw_driver_register(&nic_intel_8254x_driver);
}

KERNEL_INIT(5, nic_intel_8254x_register);
