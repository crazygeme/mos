/*
 * src/hw/hdd.c  —  ATA/IDE hard disk driver
 *
 * Supports both PIO (Programmed I/O) and Bus Master DMA modes.
 * DMA is used when the PCI IDE controller exposes a bus master I/O range
 * (PCI BAR4).  Falls back to PIO if DMA is unavailable.
 *
 * PIO:  interrupt-driven, multi-sector per operation when contiguous.
 * DMA:  PRDT-based bus master DMA with a per-channel one-page bounce buffer
 *       (no alignment constraint on caller buffer).
 *
 * Partition discovery: MBR + recursive extended partition tables.
 * Block cache: LRU write-back, indexed by 8-sector head groups.
 */
#include <int/int.h>
#include <dev/blockdev.h>
#include <hw/hdd.h>
#include <hw/pci.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <lib/list.h>
#include <lib/port.h>
#include <lib/lock.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <ps/ps.h>
#include <macro.h>
#include <config.h>
#include <ext4.h>
#include <stdint.h>

extern unsigned cache_count;

/* ── ATA command-block register addresses ────────────────────────────────────── */
#define reg_data(c) ((c)->reg_base + 0)
#define reg_error(c) ((c)->reg_base + 1)
#define reg_feature(c) ((c)->reg_base + 1)
#define reg_nsect(c) ((c)->reg_base + 2)
#define reg_lbal(c) ((c)->reg_base + 3)
#define reg_lbam(c) ((c)->reg_base + 4)
#define reg_lbah(c) ((c)->reg_base + 5)
#define reg_device(c) ((c)->reg_base + 6)
#define reg_status(c) ((c)->reg_base + 7)
#define reg_command(c) ((c)->reg_base + 7)

/* ── ATA control-block register addresses ────────────────────────────────────── */
#define reg_ctl(c) ((c)->reg_base + 0x206)
#define reg_alt_status(c) ((c)->reg_base + 0x206)

/* ── ATA status register bits ────────────────────────────────────────────────── */
#define STA_BSY 0x80
#define STA_DRDY 0x40
#define STA_DRQ 0x08
#define STA_ERR 0x01

/* ── ATA control register bits ───────────────────────────────────────────────── */
#define CTL_SRST 0x04 /* software reset */

/* ── ATA device register bits ────────────────────────────────────────────────── */
#define DEV_MBS 0xa0 /* must-be-set */
#define DEV_LBA 0x40 /* LBA addressing */
#define DEV_DEV 0x10 /* device select (0=master, 1=slave) */

/* ── ATA commands ────────────────────────────────────────────────────────────── */
#define CMD_IDENTIFY_DEVICE 0xec
#define CMD_READ_SECTORS 0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_READ_DMA 0xc8
#define CMD_WRITE_DMA 0xca

/* ── Bus Master IDE register offsets (from per-channel BM base) ─────────────── */
#define BM_CMD 0 /* byte: bit0=start, bit3=direction      */
#define BM_STATUS 2 /* byte: bit0=active, bit1=error, bit2=irq */
#define BM_PRDT 4 /* dword: physical address of PRDT        */

#define BM_CMD_START 0x01
#define BM_CMD_READ 0x08 /* device → memory (reading from disk)  */
#define BM_CMD_WRITE 0x00 /* memory → device (writing to disk)    */

#define BM_STA_ERROR 0x02
#define BM_STA_IRQ 0x04

/* ── Physical Region Descriptor Table entry ──────────────────────────────────── */
typedef struct {
	uint32_t phys_addr; /* physical address of DMA buffer        */
	uint16_t byte_count; /* byte count; 0 means 64 KiB            */
	uint16_t eot; /* bit 15 set on last (and only) entry   */
} __attribute__((packed, aligned(4))) prdt_entry_t;

#define PRDT_EOT 0x8000

/* ── ATA disk (one per channel slot) ─────────────────────────────────────────── */
typedef struct _ata_disk {
	char name[8];
	struct _channel *channel;
	int dev_no; /* 0=master, 1=slave */
	int is_ata;
} ata_disk;

/* ── ATA channel (up to two disks) ───────────────────────────────────────────── */
typedef struct _channel {
	char name[8];
	unsigned short reg_base; /* ATA command block base I/O port  */
	unsigned char irq; /* IRQ vector (PIC-remapped)        */
	mutex_t lock;
	ata_disk devices[2];

	/* Bus Master DMA (all zero when DMA unavailable) */
	unsigned short bm_base; /* BM I/O base for this channel     */
	prdt_entry_t *prdt; /* PRDT table (kernel virt addr)    */
	void *dma_buf; /* 512-byte bounce buffer (virt)    */
	unsigned int dma_buf_phys; /* physical address of dma_buf      */
} channel;

#define HDD_IO_MAX_SECTORS (PAGE_SIZE / BLOCK_SECTOR_SIZE)

/* ── Block cache structures ───────────────────────────────────────────────────── */
#if HDD_CACHE_OPEN
#define CACHE_SECTOR_COUNT ((HDD_CACHE_SIZE * PAGE_SIZE) / BLOCK_SECTOR_SIZE)
#define SECTOR_PER_PAGE (PAGE_SIZE / BLOCK_SECTOR_SIZE)
#define PREREAD_SECTOR 8
#define HEAD_SECTOR(s) ((s) / PREREAD_SECTOR * PREREAD_SECTOR)
#define SECTOR_OFF(s) ((s) - HEAD_SECTOR(s))

typedef struct _block_cache_item {
	int sector; /* head sector of this cached extent        */
	unsigned dirty; /* non-zero if write-back flush needed      */
	int loading; /* non-zero while refill/evict I/O is in flight */
	unsigned page_index; /* phymm_alloc_user() page backing buf */
	void *buf; /* kernel alias for page_index */
	struct rb_node hash_node;
	list_entry time_list;
} block_cache_item;

typedef struct _block_cache {
	hash_table *hash; /* rb-tree keyed by head sector     */
	list_entry timer_list_head; /* LRU list; head=LRU, newest at front */
	int sectors; /* total cached sectors             */
} block_cache;
#endif /* HDD_CACHE_OPEN */

/* ── Partition descriptor ─────────────────────────────────────────────────────── */
typedef struct _partition {
	ata_disk *disk;
	unsigned int start; /* first sector within the disk device  */
	unsigned char bootable;
#if HDD_CACHE_OPEN
	block_cache cache;
	int cache_inited;
	mutex_t cache_lock;
#endif
} partition;

/* ── Forward declarations ─────────────────────────────────────────────────────── */
static void interrupt_handler(intr_frame *);
static void reset_channel(channel *);
static int check_device_type(ata_disk *);
static void identify_ata_device(ata_disk *);
static int wait_while_busy(const ata_disk *);
static void select_device(const ata_disk *);
static void select_sector(ata_disk *, unsigned int sec_no, unsigned int count);
static void input_sector(channel *, void *);
static void output_sector(channel *, const void *);
static int disk_read(void *aux, unsigned sector, void *buf, unsigned len);
static int disk_write(void *aux, unsigned sector, void *buf, unsigned len);
static int partition_read(void *aux, unsigned sector, void *buf, unsigned len);
static int partition_write(void *aux, unsigned sector, void *buf, unsigned len);
static void partition_close(void *aux);
static void partition_cache_flush(void *aux);
static void parse_partition(ata_disk *, unsigned capacity);
static void read_partition_table(ata_disk *, unsigned capacity,
				 unsigned int sector,
				 unsigned int primary_extended_sector,
				 int *part_nr);
static void found_partition(ata_disk *, unsigned capacity,
			    unsigned char part_type, unsigned int start,
			    unsigned int size, int part_nr,
			    unsigned char bootable);
#if HDD_CACHE_OPEN
static int partition_cache_read(void *aux, unsigned sector, void *buf,
				unsigned len);
static int partition_cache_write(void *aux, unsigned sector, void *buf,
				 unsigned len);
#endif
static int hdd_bdev_open(struct ext4_blockdev *bdev);
static int hdd_bdev_bread(struct ext4_blockdev *bdev, void *buf,
			  uint64_t blk_id, uint32_t blk_cnt);
static int hdd_bdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			   uint64_t blk_id, uint32_t blk_cnt);
static int hdd_bdev_close(struct ext4_blockdev *bdev);
static int hdd_bdev_lock(struct ext4_blockdev *bdev);
static int hdd_bdev_unlock(struct ext4_blockdev *bdev);

/* ── Global channel array ─────────────────────────────────────────────────────── */
#define CHANNEL_CNT 2
static channel channels[CHANNEL_CNT];

typedef struct {
	char name[32];
	unsigned int size;
	unsigned char part_type;
	void *aux;
	int (*read)(void *aux, unsigned sector, void *buf, unsigned len);
	int (*write)(void *aux, unsigned sector, void *buf, unsigned len);
	struct ext4_blockdev *bdev;
} hdd_partition_info;

/* ── Private partition table ─────────────────────────────────────────────────── */
static hdd_partition_info hdd_partitions[HDD_MAX_PARTITIONS];
static int hdd_partition_count = 0;

int hdd_partition_total(void)
{
	return hdd_partition_count;
}

const char *hdd_partition_name(unsigned idx)
{
	if ((int)idx >= hdd_partition_count)
		return NULL;
	return hdd_partitions[idx].name;
}

unsigned hdd_partition_size_sectors(unsigned idx)
{
	if ((int)idx >= hdd_partition_count)
		return 0;
	return hdd_partitions[idx].size;
}

int hdd_partition_read(unsigned idx, unsigned sector, void *buf, unsigned len)
{
	hdd_partition_info *pi;

	if ((int)idx >= hdd_partition_count)
		return -1;
	pi = &hdd_partitions[idx];
	return pi->read(pi->aux, sector, buf, len);
}

int hdd_partition_write(unsigned idx, unsigned sector, void *buf, unsigned len)
{
	hdd_partition_info *pi;

	if ((int)idx >= hdd_partition_count)
		return -1;
	pi = &hdd_partitions[idx];
	return pi->write(pi->aux, sector, buf, len);
}

/* ── I/O statistics ───────────────────────────────────────────────────────────── */
unsigned disk_read_size = 0;
unsigned disk_write_size = 0;
unsigned disk_read_req_count = 0;
unsigned disk_write_req_count = 0;
unsigned disk_read_multi_req_count = 0;
unsigned disk_write_multi_req_count = 0;
unsigned disk_read_multi_sectors = 0;
unsigned disk_write_multi_sectors = 0;

/* ── PCI IDE Bus Master detection ─────────────────────────────────────────────── */
static unsigned short g_bm_base = 0;
static uint32_t g_ide_pci = 0;

static void ide_pci_scan_cb(uint32_t device, uint16_t vendor_id,
			    uint16_t device_id, void *extra)
{
	uint8_t class_code = (uint8_t)pci_read_field(device, PCI_CLASS, 1);
	uint8_t subclass = (uint8_t)pci_read_field(device, PCI_SUBCLASS, 1);
	uint32_t bar4;

	(void)vendor_id;
	(void)device_id;
	(void)extra;

	if (class_code != 0x01 || subclass != 0x01)
		return;

	bar4 = pci_read_field(device, PCI_BAR4, 4);
	if (!(bar4 & 1))
		return; /* not an I/O space BAR */

	g_bm_base = (unsigned short)(bar4 & ~0x3u);
	g_ide_pci = device;
	printk("hdd: IDE bus master I/O at 0x%x\n", g_bm_base);
}

static void dma_init_channel(channel *c, int chan_no)
{
	if (!g_bm_base)
		return;

	c->bm_base = g_bm_base + (unsigned short)(chan_no * 8);

	/*
	 * PRDT: single 8-byte entry.  kmalloc returns kernel-heap memory
	 * whose physical address is resolved from its page-table mapping. An 8-byte
	 * allocation never crosses a 64 KiB physical boundary.
	 */
	c->prdt = (prdt_entry_t *)kmalloc(sizeof(prdt_entry_t));
	memset(c->prdt, 0, sizeof(prdt_entry_t));

	/* One-page (4 KiB) bounce buffer — always page-aligned, never
	 * crosses a 64 KiB boundary, suitable for single-sector DMA.    */
	c->dma_buf = (void *)vm_alloc(1);
	c->dma_buf_phys = VIRT_TO_PHY(c->dma_buf);

	printk("hdd: %s DMA enabled, bm=0x%x buf_phys=0x%x\n", c->name,
	       c->bm_base, c->dma_buf_phys);
}

/* ── Initialisation ───────────────────────────────────────────────────────────── */
static void hdd_init(void)
{
	int chan_no, dev_no;

	/* Locate PCI IDE controller and its bus master I/O range. */
	pci_scan(ide_pci_scan_cb, PCI_SCAN_ALL, NULL);

	/* Enable bus master + I/O space on the IDE controller if found. */
	if (g_ide_pci) {
		uint32_t cmd = pci_read_field(g_ide_pci, PCI_COMMAND, 2);
		port_write_dword(PCI_ADDRESS_PORT,
				 pci_get_addr(g_ide_pci, PCI_COMMAND));
		port_write_word(PCI_VALUE_PORT, (unsigned short)(cmd | 0x05u));
	}

	for (chan_no = 0; chan_no < CHANNEL_CNT; chan_no++) {
		channel *c = &channels[chan_no];

		c->name[0] = 'i';
		c->name[1] = 'd';
		c->name[2] = 'e';
		c->name[3] = '0' + chan_no;
		c->name[4] = '\0';

		switch (chan_no) {
		case 0:
			c->reg_base = 0x1f0;
			c->irq = 14 + 0x20;
			break;
		default:
			c->reg_base = 0x170;
			c->irq = 15 + 0x20;
			break;
		}

		mutex_init(&c->lock);
		c->bm_base = 0;
		c->prdt = NULL;
		c->dma_buf = NULL;
		c->dma_buf_phys = 0;

		for (dev_no = 0; dev_no < 2; dev_no++) {
			ata_disk *d = &c->devices[dev_no];
			d->name[0] = 'h';
			d->name[1] = 'd';
			d->name[2] = 'a' + chan_no * 2 + dev_no;
			d->name[3] = '\0';
			d->channel = c;
			d->dev_no = dev_no;
			d->is_ata = 0;
		}

		int_register(c->irq, interrupt_handler, 0, 0);
		reset_channel(c);

		if (check_device_type(&c->devices[0]))
			check_device_type(&c->devices[1]);

		for (dev_no = 0; dev_no < 2; dev_no++)
			if (c->devices[dev_no].is_ata)
				identify_ata_device(&c->devices[dev_no]);

		/* Set up DMA after devices are identified. */
		dma_init_channel(c, chan_no);
	}
}

/* ── Interrupt handler ────────────────────────────────────────────────────────── */
static void interrupt_handler(intr_frame *f)
{
	(void)f;
	/* ACK is done in the polling path; nothing to do here. */
}

/* ── Channel reset and device detection ──────────────────────────────────────── */
static void reset_channel(channel *c)
{
	int present[2], dev_no, i;

	for (dev_no = 0; dev_no < 2; dev_no++) {
		ata_disk *d = &c->devices[dev_no];
		select_device(d);
		port_write_byte(reg_nsect(c), 0x55);
		port_write_byte(reg_lbal(c), 0xaa);
		port_write_byte(reg_nsect(c), 0xaa);
		port_write_byte(reg_lbal(c), 0x55);
		port_write_byte(reg_nsect(c), 0x55);
		port_write_byte(reg_lbal(c), 0xaa);
		present[dev_no] = (port_read_byte(reg_nsect(c)) == 0x55 &&
				   port_read_byte(reg_lbal(c)) == 0xaa);
	}

	/* Assert SRST then release; enables interrupts as a side effect. */
	port_write_byte(reg_ctl(c), 0);
	usleep(10);
	port_write_byte(reg_ctl(c), CTL_SRST);
	usleep(10);
	port_write_byte(reg_ctl(c), 0);
	delay(150000);

	if (present[0]) {
		select_device(&c->devices[0]);
		wait_while_busy(&c->devices[0]);
	}

	if (present[1]) {
		select_device(&c->devices[1]);
		for (i = 0; i < 3000; i++) {
			if (port_read_byte(reg_nsect(c)) == 1 &&
			    port_read_byte(reg_lbal(c)) == 1)
				break;
			delay(10000);
		}
		wait_while_busy(&c->devices[1]);
	}
}

static int check_device_type(ata_disk *d)
{
	channel *c = d->channel;
	unsigned char error, lbam, lbah, status;

	select_device(d);
	error = port_read_byte(reg_error(c));
	lbam = port_read_byte(reg_lbam(c));
	lbah = port_read_byte(reg_lbah(c));
	status = port_read_byte(reg_status(c));

	if ((error != 1 && (error != 0x81 || d->dev_no == 1)) ||
	    !(status & STA_DRDY) || (status & STA_BSY)) {
		d->is_ata = 0;
		return error != 0x81;
	}
	d->is_ata = (lbam == 0 && lbah == 0) || (lbam == 0x3c && lbah == 0xc3);
	return 1;
}

static void identify_ata_device(ata_disk *d)
{
	channel *c = d->channel;
	char id[BLOCK_SECTOR_SIZE];
	unsigned int capacity;

	select_device(d);
	port_write_byte(reg_command(c), CMD_IDENTIFY_DEVICE);

	if (!wait_while_busy(d)) {
		port_read_byte(reg_status(c)); /* ack */
		d->is_ata = 0;
		return;
	}
	port_read_byte(reg_status(c)); /* ack */
	input_sector(c, id);

	capacity = *(unsigned int *)&id[60 * 2];
	printk("hdd: %s: %u sectors (%u MiB)\n", d->name, capacity,
	       capacity / 2048);

	parse_partition(d, capacity);
}

/* ── Low-level PIO primitives ─────────────────────────────────────────────────── */
static int wait_while_busy(const ata_disk *d)
{
	channel *c = d->channel;
	int i;
	for (i = 0; i < 3000; i++) {
		unsigned char s = port_read_byte(reg_alt_status(c));
		if (!(s & STA_BSY))
			return (s & STA_DRQ) != 0;
		delay(10000);
	}
	klog("%s: wait_while_busy timeout\n", d->name);
	return 0;
}

static void select_device(const ata_disk *d)
{
	channel *c = d->channel;
	unsigned char dev = DEV_MBS;
	if (d->dev_no == 1)
		dev |= DEV_DEV;
	port_write_byte(reg_device(c), dev);
	port_read_byte(reg_alt_status(c)); /* 400 ns settling delay */
}

static void select_sector(ata_disk *d, unsigned int sec_no, unsigned int count)
{
	channel *c = d->channel;
	select_device(d);
	port_write_byte(reg_nsect(c), (unsigned char)count);
	port_write_byte(reg_lbal(c), (unsigned char)sec_no);
	port_write_byte(reg_lbam(c), (unsigned char)(sec_no >> 8));
	port_write_byte(reg_lbah(c), (unsigned char)(sec_no >> 16));
	port_write_byte(reg_device(c), DEV_MBS | DEV_LBA |
					       (d->dev_no == 1 ? DEV_DEV : 0) |
					       (unsigned char)(sec_no >> 24));
}

static void input_sector(channel *c, void *buf)
{
	port_read_dwords(reg_data(c), buf, BLOCK_SECTOR_SIZE / 4);
}

static void output_sector(channel *c, const void *buf)
{
	port_write_dwords(reg_data(c), buf, BLOCK_SECTOR_SIZE / 4);
}

static unsigned int disk_io_chunk_sectors(unsigned len)
{
	unsigned int sectors;

	if (!len || (len % BLOCK_SECTOR_SIZE) != 0)
		return 0;

	sectors = len / BLOCK_SECTOR_SIZE;
	if (sectors > HDD_IO_MAX_SECTORS)
		sectors = HDD_IO_MAX_SECTORS;
	return sectors;
}

/* ── Disk I/O — DMA preferred, PIO fallback ──────────────────────────────────── */
static int disk_read(void *aux, unsigned sec_no, void *buf, unsigned len)
{
	ata_disk *d = aux;
	channel *c = d->channel;
	unsigned char *dst = buf;
	unsigned remaining = len;

	disk_read_req_count++;
	mutex_lock(&c->lock);

	while (remaining) {
		unsigned int sectors = disk_io_chunk_sectors(remaining);
		unsigned int chunk_bytes;
		unsigned int i;

		if (!sectors) {
			mutex_unlock(&c->lock);
			return -1;
		}

		chunk_bytes = sectors * BLOCK_SECTOR_SIZE;
		if (sectors > 1) {
			disk_read_multi_req_count++;
			disk_read_multi_sectors += sectors;
		}
		select_sector(d, sec_no, sectors);

		if (c->bm_base) {
			/* ── DMA path: device → bounce buffer → caller ── */
			c->prdt->phys_addr = c->dma_buf_phys;
			c->prdt->byte_count = chunk_bytes;
			c->prdt->eot = PRDT_EOT;

			/* Clear error/IRQ status bits (write-1-to-clear). */
			port_write_byte(c->bm_base + BM_STATUS,
					BM_STA_IRQ | BM_STA_ERROR);
			/* Load PRDT physical address. */
			port_write_dword(c->bm_base + BM_PRDT,
					 VIRT_TO_PHY(c->prdt));
			/* Set transfer direction: device → memory. */
			port_write_byte(c->bm_base + BM_CMD, BM_CMD_READ);
			/* Issue ATA DMA read command. */
			port_write_byte(reg_command(c), CMD_READ_DMA);
			/* Start bus master. */
			port_write_byte(c->bm_base + BM_CMD,
					BM_CMD_READ | BM_CMD_START);

			/* Poll until DMA engine finishes (active bit clears). */
			while (port_read_byte(c->bm_base + BM_STATUS) & 0x01)
				;
			port_write_byte(c->bm_base + BM_CMD, 0);

			/* Ack ATA interrupt, then read and clear BM status. */
			port_read_byte(reg_status(c));
			{
				unsigned char bm_sta =
					port_read_byte(c->bm_base + BM_STATUS);
				port_write_byte(c->bm_base + BM_STATUS,
						BM_STA_IRQ | BM_STA_ERROR);
				if (bm_sta & BM_STA_ERROR)
					klog("%s: DMA read error sector=%u"
					     " count=%u\n",
					     d->name, sec_no, sectors);
			}
			memcpy(dst, c->dma_buf, chunk_bytes);
		} else {
			/* ── PIO path ── */
			port_write_byte(reg_command(c), CMD_READ_SECTORS);

			for (i = 0; i < sectors; i++) {
				if (!wait_while_busy(d))
					klog("%s: PIO read timeout sector=%u\n",
					     d->name, sec_no + i);

				port_read_byte(reg_status(c)); /* ack */
				input_sector(c, dst + i * BLOCK_SECTOR_SIZE);
			}
		}

		dst += chunk_bytes;
		sec_no += sectors;
		remaining -= chunk_bytes;
	}

	mutex_unlock(&c->lock);
	disk_read_size += len;
	return len;
}

static int disk_write(void *aux, unsigned sec_no, void *buf, unsigned len)
{
	ata_disk *d = aux;
	channel *c = d->channel;
	const unsigned char *src = buf;
	unsigned remaining = len;

	disk_write_req_count++;
	mutex_lock(&c->lock);

	while (remaining) {
		unsigned int sectors = disk_io_chunk_sectors(remaining);
		unsigned int chunk_bytes;
		unsigned int i;

		if (!sectors) {
			mutex_unlock(&c->lock);
			return -1;
		}

		chunk_bytes = sectors * BLOCK_SECTOR_SIZE;
		if (sectors > 1) {
			disk_write_multi_req_count++;
			disk_write_multi_sectors += sectors;
		}
		select_sector(d, sec_no, sectors);

		if (c->bm_base) {
			/* ── DMA path: caller → bounce buffer → device ── */
			memcpy(c->dma_buf, src, chunk_bytes);

			c->prdt->phys_addr = c->dma_buf_phys;
			c->prdt->byte_count = chunk_bytes;
			c->prdt->eot = PRDT_EOT;

			port_write_byte(c->bm_base + BM_STATUS,
					BM_STA_IRQ | BM_STA_ERROR);

			port_write_dword(c->bm_base + BM_PRDT,
					 VIRT_TO_PHY(c->prdt));
			/* Set transfer direction: memory → device. */
			port_write_byte(c->bm_base + BM_CMD, BM_CMD_WRITE);
			/* Issue ATA DMA write command. */
			port_write_byte(reg_command(c), CMD_WRITE_DMA);
			/* Start bus master. */
			port_write_byte(c->bm_base + BM_CMD,
					BM_CMD_WRITE | BM_CMD_START);

			/* Poll until DMA engine finishes (active bit clears). */
			while (port_read_byte(c->bm_base + BM_STATUS) & 0x01)
				;
			port_write_byte(c->bm_base + BM_CMD, 0);

			/* Ack ATA interrupt, then read and clear BM status. */
			port_read_byte(reg_status(c));
			{
				unsigned char bm_sta =
					port_read_byte(c->bm_base + BM_STATUS);
				port_write_byte(c->bm_base + BM_STATUS,
						BM_STA_IRQ | BM_STA_ERROR);
				if (bm_sta & BM_STA_ERROR)
					klog("%s: DMA write error sector=%u"
					     " count=%u\n",
					     d->name, sec_no, sectors);
			}
		} else {
			/*
			 * ── PIO path ──
			 * After CMD_WRITE_SECTORS the device asserts DRQ when
			 * ready for each sector in the contiguous run.
			 */
			port_write_byte(reg_command(c), CMD_WRITE_SECTORS);

			for (i = 0; i < sectors; i++) {
				if (!wait_while_busy(d))
					klog("%s: PIO write DRQ timeout"
					     " sector=%u\n",
					     d->name, sec_no + i);
				output_sector(c, src + i * BLOCK_SECTOR_SIZE);
			}

			/* Poll for write completion (BSY clears), then ack. */
			wait_while_busy(d);
			port_read_byte(reg_status(c)); /* ack */
		}

		src += chunk_bytes;
		sec_no += sectors;
		remaining -= chunk_bytes;
	}

	mutex_unlock(&c->lock);
	disk_write_size += len;
	return len;
}

/* ── Partition-level I/O ──────────────────────────────────────────────────────── */
static int partition_read(void *aux, unsigned sector, void *buf, unsigned len)
{
	partition *p = aux;
	return disk_read(p->disk, p->start + sector, buf, len);
}

static int partition_write(void *aux, unsigned sector, void *buf, unsigned len)
{
	partition *p = aux;
	return disk_write(p->disk, p->start + sector, buf, len);
}

/* ── Block cache ──────────────────────────────────────────────────────────────── */
#if HDD_CACHE_OPEN
unsigned hdd_cache_hit = 0;
unsigned hdd_cache_search_count = 0;
unsigned hdd_cache_size = 0;
unsigned hdd_cache_max_size = 0;

static int int_comp(const void *key1, const void *key2)
{
	return (int)key1 - (int)key2;
}

static block_cache_item *block_cache_item_create(void)
{
	unsigned buf_pages = BLOCK_SECTOR_SIZE * PREREAD_SECTOR / PAGE_SIZE;
	unsigned phy;
	block_cache_item *item;

	if (buf_pages != 1)
		return NULL;

	item = kmalloc(sizeof(*item));
	if (!item)
		return NULL;
	item->sector = -1;
	item->dirty = 0;
	item->loading = 0;
	item->page_index = phymm_alloc_user();
	if (item->page_index == PHYMM_INVALID) {
		kfree(item);
		return NULL;
	}

	phy = item->page_index * PAGE_SIZE;
	if (mm_kmap_phys(phy) != 1) {
		phymm_free_user(item->page_index);
		kfree(item);
		return NULL;
	}

	item->buf = (void *)PHY_TO_VIRT(phy);
	if (!item->buf) {
		phymm_free_user(item->page_index);
		kfree(item);
		return NULL;
	}

	phymm_reference_page(item->page_index);
	cache_count += buf_pages;
	rb_init_node(&item->hash_node);
	list_init(&item->time_list);
	return item;
}

static void block_cache_item_remove(block_cache_item *item)
{
	unsigned buf_pages = BLOCK_SECTOR_SIZE * PREREAD_SECTOR / PAGE_SIZE;
	if (!buf_pages)
		buf_pages = 1;
	if (!item)
		return;
	if (item->page_index != PHYMM_INVALID &&
	    phymm_dereference_page(item->page_index) == 0)
		phymm_free_user(item->page_index);
	cache_count -= buf_pages;
	kfree(item);
}

static void init_partition_cache(partition *p)
{
	p->cache.sectors = 0;
	p->cache.hash = hash_create(int_comp, NULL);
	list_init(&p->cache.timer_list_head);
	p->cache_inited = 1;
	mutex_init(&p->cache_lock);
}

static block_cache_item *hdd_cache_lookup(partition *p, int sector)
{
	int head_sector = HEAD_SECTOR(sector);
	key_value_pair *pair;

	if (sector == -1)
		return (block_cache_item *)-1;

	hdd_cache_search_count++;

	pair = hash_find(p->cache.hash, head_sector);

	if (!pair)
		return NULL;

	hdd_cache_hit++;
	return (block_cache_item *)pair->val;
}

static block_cache_item *hdd_cache_find_oldest(partition *p)
{
	list_entry *node;
	if (p->cache.sectors == 0)
		return NULL;
	/* LRU is at the tail of the list (prev of sentinel). */
	node = p->cache.timer_list_head.prev;
	return container_of(node, block_cache_item, time_list);
}

static void hdd_cache_flush_buf(partition *p, int head_sector, void *buf,
				int dirty)
{
	if (head_sector < 0 || !dirty)
		return;
	partition_write(p, head_sector, buf,
			PREREAD_SECTOR * BLOCK_SECTOR_SIZE);
}

static void hdd_cache_flush(partition *p, block_cache_item *item)
{
	if (item->sector < 0 || !item->dirty)
		return;
	hdd_cache_flush_buf(p, item->sector, item->buf, item->dirty);
	item->dirty = 0;
}

static int hdd_cache_fill_line(void *aux, int head_sector, void *buf)
{
	return partition_read(aux, head_sector, buf,
			      PREREAD_SECTOR * BLOCK_SECTOR_SIZE) ==
			       PREREAD_SECTOR * BLOCK_SECTOR_SIZE ?
		       0 :
		       -1;
}

static block_cache_item *hdd_cache_reserve_miss_locked(partition *p,
						       int head_sector,
						       int *old_sector,
						       int *old_dirty)
{
	block_cache_item *item;

	if (p->cache.sectors < CACHE_SECTOR_COUNT) {
		item = block_cache_item_create();
		if (!item)
			return NULL;
		p->cache.sectors += PREREAD_SECTOR;
		hdd_cache_size = p->cache.sectors;
		if (hdd_cache_max_size < p->cache.sectors)
			hdd_cache_max_size = p->cache.sectors;
		list_insert_head(&p->cache.timer_list_head, &item->time_list);
	} else {
		item = hdd_cache_find_oldest(p);
		if (!item)
			return NULL;
	}

	*old_sector = item->sector;
	*old_dirty = item->dirty;
	if (item->sector >= 0)
		hash_remove(p->cache.hash, item->sector);

	item->sector = head_sector;
	item->dirty = 0;
	item->loading = 1;
	list_remove_entry(&item->time_list);
	list_insert_head(&p->cache.timer_list_head, &item->time_list);
	hash_insert(p->cache.hash, head_sector, item);
	return item;
}

static void hdd_cache_publish_loaded_locked(block_cache_item *item)
{
	if (!item)
		return;
	item->loading = 0;
}

static void hdd_cache_cancel_load_locked(partition *p, block_cache_item *item)
{
	if (!item)
		return;
	hash_remove(p->cache.hash, item->sector);
	item->sector = -1;
	item->dirty = 0;
	item->loading = 0;
	list_remove_entry(&item->time_list);
	list_insert_tail(&p->cache.timer_list_head, &item->time_list);
}

static void hdd_cache_update(partition *p, block_cache_item *item, int sector,
			     void *buf, int mark_dirty)
{
	int head_sector = HEAD_SECTOR(sector);
	int sector_off = SECTOR_OFF(sector);

	if (item->sector != head_sector) {
		hash_remove(p->cache.hash, item->sector);
		hash_insert(p->cache.hash, head_sector, item);
	}
	item->dirty |= mark_dirty;
	item->sector = head_sector;
	list_remove_entry(&item->time_list);
	list_insert_head(&p->cache.timer_list_head, &item->time_list);

	memcpy((char *)item->buf + sector_off * BLOCK_SECTOR_SIZE, buf,
	       BLOCK_SECTOR_SIZE);

#if HDD_CACHE_WRITE_POLICY == HDD_CACHE_WRITE_THOUGH
	if (mark_dirty)
		hdd_cache_flush(p, item);
#endif
}

static void partition_cache_evict(partition *p)
{
	list_entry *entry, *next;
	mutex_lock(&p->cache_lock);
	entry = p->cache.timer_list_head.prev;
	while (entry != &p->cache.timer_list_head) {
		next = entry->prev;
		block_cache_item *item =
			container_of(entry, block_cache_item, time_list);
		block_cache_item_remove(item);
		p->cache.sectors -= PREREAD_SECTOR;
		hdd_cache_size = p->cache.sectors;
		entry = next;
	}
	mutex_unlock(&p->cache_lock);
}

unsigned hdd_cache_read_size = 0;
unsigned hdd_cache_write_size = 0;

static int partition_cache_read(void *aux, unsigned sector, void *buf,
				unsigned len)
{
	partition *p = aux;
	block_cache_item *volatile item;
	int sector_off = SECTOR_OFF(sector);
	int head_sector = HEAD_SECTOR(sector);
	int old_sector;
	int old_dirty;

	(void)len;

retry:

	mutex_lock(&p->cache_lock);

	item = hdd_cache_lookup(p, sector);
	if (item) {
		if (item->loading) {
			mutex_unlock(&p->cache_lock);
			task_sched();
			goto retry;
		}
		/* Cache hit: promote to MRU and copy out. */
		list_remove_entry(&item->time_list);
		list_insert_head(&p->cache.timer_list_head, &item->time_list);
		mutex_unlock(&p->cache_lock);
		memcpy(buf, (char *)item->buf + sector_off * BLOCK_SECTOR_SIZE,
		       BLOCK_SECTOR_SIZE);
		hdd_cache_read_size += BLOCK_SECTOR_SIZE;
		return BLOCK_SECTOR_SIZE;
	}

	/* Cache miss: reserve a slot under lock, then perform disk I/O unlocked. */
	item = hdd_cache_reserve_miss_locked(p, head_sector, &old_sector,
					     &old_dirty);
	mutex_unlock(&p->cache_lock);

	if (!item)
		return -1;

	hdd_cache_flush_buf(p, old_sector, item->buf, old_dirty);
	if (hdd_cache_fill_line(aux, head_sector, item->buf) != 0) {
		mutex_lock(&p->cache_lock);
		hdd_cache_cancel_load_locked(p, item);
		mutex_unlock(&p->cache_lock);
		return -1;
	}

	mutex_lock(&p->cache_lock);
	hdd_cache_publish_loaded_locked(item);
	mutex_unlock(&p->cache_lock);

	memcpy(buf, (char *)item->buf + sector_off * BLOCK_SECTOR_SIZE,
	       BLOCK_SECTOR_SIZE);
	hdd_cache_read_size += BLOCK_SECTOR_SIZE;
	return BLOCK_SECTOR_SIZE;
}

static int partition_cache_write(void *aux, unsigned sector, void *buf,
				 unsigned len)
{
	partition *p = aux;
	block_cache_item *item;
	int head_sector = HEAD_SECTOR(sector);
	int old_sector;
	int old_dirty;

retry:

	mutex_lock(&p->cache_lock);

	item = hdd_cache_lookup(p, sector);
	if (item) {
		if (item->loading) {
			mutex_unlock(&p->cache_lock);
			task_sched();
			goto retry;
		}
		hdd_cache_update(p, item, sector, buf, 1);
		mutex_unlock(&p->cache_lock);
		hdd_cache_write_size += BLOCK_SECTOR_SIZE;
		return BLOCK_SECTOR_SIZE;
	}

	/*
	 * Write miss: reserve a line, read the full extent so untouched sectors
	 * remain valid, then patch the written sector into the cached line.
	 */
	item = hdd_cache_reserve_miss_locked(p, head_sector, &old_sector,
					     &old_dirty);
	mutex_unlock(&p->cache_lock);

	if (!item)
		return -1;

	hdd_cache_flush_buf(p, old_sector, item->buf, old_dirty);
	if (hdd_cache_fill_line(aux, head_sector, item->buf) != 0) {
		mutex_lock(&p->cache_lock);
		hdd_cache_cancel_load_locked(p, item);
		mutex_unlock(&p->cache_lock);
		return -1;
	}

	mutex_lock(&p->cache_lock);
	hdd_cache_publish_loaded_locked(item);
	hdd_cache_update(p, item, sector, buf, 1);
	mutex_unlock(&p->cache_lock);
	hdd_cache_write_size += len;
	return len;
}

static void partition_cache_flush(void *aux)
{
	partition *p = aux;
	list_entry *entry, *next;

	if (!p->cache_inited)
		return;

	mutex_lock(&p->cache_lock);
	entry = p->cache.timer_list_head.prev;
	while (entry != &p->cache.timer_list_head) {
		next = entry->prev;
		block_cache_item *item =
			container_of(entry, block_cache_item, time_list);
		if (item && item->sector != -1 && item->dirty) {
			hdd_cache_flush(p, item);
			item->dirty = 0;
		}
		entry = next;
	}
	mutex_unlock(&p->cache_lock);
}

#else /* !HDD_CACHE_OPEN */

static void partition_cache_flush(void *aux)
{
	(void)aux;
}

#endif /* HDD_CACHE_OPEN */

/* ── Partition close (flush + evict cache) ────────────────────────────────────── */
static void partition_close(void *aux)
{
#if HDD_CACHE_OPEN
	partition *p = aux;
	if (p->cache_inited) {
		partition_cache_flush(aux);
		partition_cache_evict(aux);
		hash_destroy(p->cache.hash);
		p->cache.hash = NULL;
		list_init(&p->cache.timer_list_head);
		p->cache_inited = 0;
	}
#else
	(void)aux;
#endif
}

/* ── lwext4 block device callbacks ───────────────────────────────────────────── */
static int hdd_bdev_open(struct ext4_blockdev *bdev)
{
	(void)bdev;
	return 0;
}

static int hdd_bdev_bread(struct ext4_blockdev *bdev, void *buf,
			  uint64_t blk_id, uint32_t blk_cnt)
{
	partition *p = bdev->aux;
	char *dst = (char *)buf;
	uint32_t i;

	for (i = 0; i < blk_cnt;)
#if HDD_CACHE_OPEN
	{
		partition_cache_read(p, (unsigned)(blk_id + i),
				     dst + i * BLOCK_SECTOR_SIZE,
				     BLOCK_SECTOR_SIZE);
		i++;
	}
#else
	{
		uint32_t chunk = blk_cnt - i;

		if (chunk > HDD_IO_MAX_SECTORS)
			chunk = HDD_IO_MAX_SECTORS;
		partition_read(p, (unsigned)(blk_id + i),
			       dst + i * BLOCK_SECTOR_SIZE,
			       chunk * BLOCK_SECTOR_SIZE);
		i += chunk;
	}
#endif
	return 0;
}

static int hdd_bdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			   uint64_t blk_id, uint32_t blk_cnt)
{
	partition *p = bdev->aux;
	char *src = (char *)buf;
	uint32_t i;

	for (i = 0; i < blk_cnt;)
#if HDD_CACHE_OPEN
	{
		partition_cache_write(p, (unsigned)(blk_id + i),
				      src + i * BLOCK_SECTOR_SIZE,
				      BLOCK_SECTOR_SIZE);
		i++;
	}
#else
	{
		uint32_t chunk = blk_cnt - i;

		if (chunk > HDD_IO_MAX_SECTORS)
			chunk = HDD_IO_MAX_SECTORS;
		partition_write(p, (unsigned)(blk_id + i),
				src + i * BLOCK_SECTOR_SIZE,
				chunk * BLOCK_SECTOR_SIZE);
		i += chunk;
	}
#endif
	return 0;
}

static int hdd_bdev_close(struct ext4_blockdev *bdev)
{
	/* No-op: bdev+bdif are owned by hdd_partition_info and live
	 * for the lifetime of the partition, like Linux's block_device.
	 * lwext4 calls this via ph_refctr; we simply allow remount by
	 * letting ph_refctr drop to 0 without freeing anything. */
	(void)bdev;
	return 0;
}

static int hdd_bdev_lock(struct ext4_blockdev *bdev)
{
	(void)bdev;
	return 0;
}
static int hdd_bdev_unlock(struct ext4_blockdev *bdev)
{
	(void)bdev;
	return 0;
}

/* ── Public cache flush / shutdown ───────────────────────────────────────────── */
void hdd_flush(void)
{
	int i;
	for (i = 0; i < hdd_partition_count; i++)
		if (hdd_partitions[i].aux)
			partition_cache_flush(hdd_partitions[i].aux);
}

void hdd_close(void)
{
	int i;
	for (i = 0; i < hdd_partition_count; i++)
		if (hdd_partitions[i].aux)
			partition_close(hdd_partitions[i].aux);
}

/* ── Partition discovery ──────────────────────────────────────────────────────── */
static void found_partition(ata_disk *disk, unsigned capacity,
			    unsigned char part_type, unsigned int start,
			    unsigned int size, int part_nr,
			    unsigned char bootable)
{
	partition *p;
	char name[16], ext[2];
	unsigned z;

	if (start >= capacity) {
		klog("%s%d: partition starts past end of device (sector %u)\n",
		     disk->name, part_nr, start);
		return;
	}
	if (start + size < start || start + size > capacity) {
		klog("%s%d: partition end (%u) past end of device (%u)\n",
		     disk->name, part_nr, start + size, capacity);
		return;
	}

	z = sizeof(*p) / PAGE_SIZE + 1;
	p = (partition *)vm_alloc(z);
	if (!p) {
		klog("hdd: failed to allocate partition descriptor\n");
		return;
	}
	p->disk = disk;
	p->start = start;
	p->bootable = bootable;

	strcpy(name, disk->name);
	ext[0] = '0' + part_nr;
	ext[1] = '\0';
	strcat(name, ext);

#if HDD_CACHE_OPEN
	p->cache_inited = 0;
	init_partition_cache(p);
#endif

	if (hdd_partition_count < HDD_MAX_PARTITIONS) {
		hdd_partition_info *pi = &hdd_partitions[hdd_partition_count++];
		struct ext4_blockdev_iface *bdif;
		struct ext4_blockdev *bdev;

		strcpy(pi->name, name);
		pi->size = size;
		pi->part_type = part_type;
		pi->aux = p;
#if HDD_CACHE_OPEN
		pi->read = partition_cache_read;
		pi->write = partition_cache_write;
#else
		pi->read = partition_read;
		pi->write = partition_write;
#endif

		/* Allocate and register the block device once, permanently.
		 * Like Linux drivers/block: the block_device lives as long as
		 * the partition.  hdd_bdev_close is a no-op so lwext4's
		 * ph_refctr cycles without ever freeing this struct. */
		bdif = (struct ext4_blockdev_iface *)kmalloc(sizeof(*bdif));
		bdev = (struct ext4_blockdev *)kmalloc(sizeof(*bdev));
		memset(bdif, 0, sizeof(*bdif));
		bdif->open = hdd_bdev_open;
		bdif->bread = hdd_bdev_bread;
		bdif->bwrite = hdd_bdev_bwrite;
		bdif->close = hdd_bdev_close;
		bdif->lock = hdd_bdev_lock;
		bdif->unlock = hdd_bdev_unlock;
		bdif->ph_bsize = BLOCK_SECTOR_SIZE;
		bdif->ph_bcnt = pi->size;
		bdif->ph_bbuf = (uint8_t *)kmalloc(BLOCK_SECTOR_SIZE);
		memset(bdev, 0, sizeof(*bdev));
		bdev->bdif = bdif;
		bdev->part_offset = 0;
		bdev->part_size = (uint64_t)BLOCK_SECTOR_SIZE * pi->size;
		bdev->aux = pi->aux;
		pi->bdev = bdev;
		ext4_device_register(bdev, NULL, name);
		blockdev_register(name, 3, hdd_partition_count,
				  (uint64_t)pi->size * BLOCK_SECTOR_SIZE,
				  BLOCKDEV_FLAG_MOUNTABLE);
	}
}

static void read_partition_table(ata_disk *disk, unsigned capacity,
				 unsigned int sector,
				 unsigned int primary_extended_sector,
				 int *part_nr)
{
	struct partition_table_entry {
		unsigned char bootable;
		unsigned char start_chs[3];
		unsigned char type;
		unsigned char end_chs[3];
		unsigned int offset;
		unsigned int size;
	} __attribute__((packed));

	struct partition_table {
		unsigned char loader[446];
		struct partition_table_entry partitions[4];
		unsigned short signature;
	} __attribute__((packed));

	struct partition_table *pt;
	unsigned i;

	if (sector >= capacity) {
		klog("%s: partition table at sector %u past end of device\n",
		     disk->name, sector);
		return;
	}

	pt = (struct partition_table *)kmalloc(sizeof(*pt));
	if (!pt) {
		klog("hdd: out of memory for partition table\n");
		return;
	}
	disk_read(disk, sector, pt, BLOCK_SECTOR_SIZE);

	for (i = 0; i < 4; i++) {
		struct partition_table_entry *e = &pt->partitions[i];
		if (e->size == 0 || e->type == 0)
			continue;

		if (e->type == 0x05 || e->type == 0x0f || e->type == 0x85 ||
		    e->type == 0xc5) {
			klog("%s: extended partition in sector %u\n",
			     disk->name, sector);
			if (sector == 0)
				read_partition_table(disk, capacity, e->offset,
						     e->offset, part_nr);
			else
				read_partition_table(
					disk, capacity,
					e->offset + primary_extended_sector,
					primary_extended_sector, part_nr);
		} else {
			++*part_nr;
			found_partition(disk, capacity, e->type,
					e->offset + sector, e->size, *part_nr,
					e->bootable);
		}
	}

	if (!*part_nr) {
		/* No partition table found: treat whole disk as one partition. */
		found_partition(disk, capacity, 0x21, sector, capacity, 0, 1);
	}

	kfree(pt);
}

static void parse_partition(ata_disk *disk, unsigned capacity)
{
	int count = 0;
	read_partition_table(disk, capacity, 0, 0, &count);
}

KERNEL_INIT(2, hdd_init);
