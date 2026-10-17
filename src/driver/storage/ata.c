#include <fs/fcntl.h>
#include <fs/entries.h>
#include <int/int.h>
#include <device/blockdev.h>
#include <driver/driver.h>
#include <errno.h>
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
#include <stdint.h>
#include <fs/fs.h>
#include <fs/ioctl.h>
#include <fs/vfs.h>
#include <device/devnode.h>
#include <unistd.h>
#include <lib/command.h>
#include <device/devnums.h>

#include "fixed.h"
/*
 * src/driver/storage/ata.c  —  ATA/IDE hard disk driver
 *
 * Supports both PIO (Programmed I/O) and Bus Master DMA modes.
 * DMA is used when the PCI IDE controller exposes a bus master I/O range
 * (PCI BAR4).  Falls back to PIO if DMA is unavailable.
 *
 * PIO:  interrupt-driven, multi-sector per operation when contiguous.
 * DMA:  PRDT-based bus master DMA with a per-channel aligned bounce buffer
 *       (no alignment constraint on caller buffer).
 *
 * Partition discovery and block nodes are provided by the fixed-disk layer.
 * Block caching is provided by the fixed-disk layer.
 */
#define BLOCK_SECTOR_SIZE 512

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
	void *dma_buf; /* contiguous DMA bounce buffer */
	unsigned int dma_buf_phys; /* physical address of dma_buf      */
} channel;

#if __SIZEOF_POINTER__ == 8
#define HDD_DMA_PAGES 8
#else
#define HDD_DMA_PAGES 1
#endif
#define HDD_IO_MAX_SECTORS (HDD_DMA_PAGES * PAGE_SIZE / BLOCK_SECTOR_SIZE)

/* ── Partition descriptor ─────────────────────────────────────────────────────── */
typedef struct _partition {
	ata_disk *disk;
	unsigned int start; /* first sector within the disk device  */
	unsigned int sector_count;
	unsigned char bootable;

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
static void parse_partition(ata_disk *, unsigned capacity);

#define CHANNEL_CNT 2
static channel channels[CHANNEL_CNT];

static unsigned ata_volume_count;

/* ── I/O statistics ───────────────────────────────────────────────────────────── */
static unsigned disk_read_size = 0;
static unsigned disk_write_size = 0;
unsigned disk_read_req_count = 0;
unsigned disk_write_req_count = 0;
unsigned disk_read_multi_req_count = 0;
unsigned disk_write_multi_req_count = 0;
unsigned disk_read_multi_sectors = 0;
unsigned disk_write_multi_sectors = 0;

/* ── PCI IDE Bus Master detection ─────────────────────────────────────────────── */
static unsigned short g_bm_base = 0;
static uint32_t g_ide_pci = 0;

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

	/* Power-of-two buddy alignment keeps the buffer within a 64 KiB boundary. */
	c->dma_buf = (void *)vm_alloc_dma(HDD_DMA_PAGES);
	if (!c->dma_buf) {
		kfree(c->prdt);
		c->prdt = NULL;
		c->bm_base = 0;
		return;
	}
	c->dma_buf_phys = VIRT_TO_PHY(c->dma_buf);

	printk("hdd: %s DMA enabled, bm=0x%x buf_phys=0x%x\n", c->name,
	       c->bm_base, c->dma_buf_phys);
}

/* ── Initialisation ───────────────────────────────────────────────────────────── */
static void hdd_init(void)
{
	int chan_no, dev_no;

	/* Enable I/O decoding and bus mastering when DMA is available. */
	{
		uint32_t cmd = pci_read_field(g_ide_pci, PCI_COMMAND, 2);
		pci_write_field(g_ide_pci, PCI_COMMAND, 2,
				cmd | 1U | (g_bm_base ? 4U : 0));
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
	char *id = kmalloc(BLOCK_SECTOR_SIZE);
	unsigned int capacity;

	if (!id) {
		d->is_ata = 0;
		return;
	}
	select_device(d);
	port_write_byte(reg_command(c), CMD_IDENTIFY_DEVICE);

	if (!wait_while_busy(d)) {
		port_read_byte(reg_status(c)); /* ack */
		d->is_ata = 0;
		kfree(id);
		return;
	}
	port_read_byte(reg_status(c)); /* ack */
	input_sector(c, id);

	capacity = *(unsigned int *)&id[60 * 2];
	kfree(id);
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

/* The shared fixed-disk layer discovers partitions and publishes device nodes. */
static int ata_fixed_rw(struct fixed_disk *volume, void *buffer,
			uint64_t sector, unsigned count, int write)
{
	partition *p = volume->private;
	if (sector > volume->sectors || count > volume->sectors - sector)
		return -EIO;
	unsigned done = 0;
	while (done < count) {
		unsigned chunk = count - done;
		if (chunk > HDD_IO_MAX_SECTORS)
			chunk = HDD_IO_MAX_SECTORS;
		int ret = write ? partition_write(p, (unsigned)sector + done,
						  (char *)buffer + done * 512,
						  chunk * 512) :
				  partition_read(p, (unsigned)sector + done,
						 (char *)buffer + done * 512,
						 chunk * 512);
		if (ret != (int)(chunk * 512))
			return -EIO;
		done += chunk;
	}
	return 0;
}
static int ata_fixed_flush(struct fixed_disk *volume)
{
	partition *p = volume->private;
	channel *c = p->disk->channel;
	mutex_lock(&c->lock);
	select_device(p->disk);
	int result = 0;
	uint64_t end = time_deadline_ms(30000);
	while (port_read_byte(reg_alt_status(c)) & STA_BSY)
		if (time_now_ms() >= end) {
			result = -EIO;
			break;
		}
	if (!result) {
		port_write_byte(reg_command(c), 0xe7);
		usleep(10);
		end = time_deadline_ms(30000);
		while (port_read_byte(reg_alt_status(c)) & STA_BSY)
			if (time_now_ms() >= end) {
				result = -EIO;
				break;
			}
		if (port_read_byte(reg_status(c)) & (STA_ERR | 0x20))
			result = -EIO;
	}
	mutex_unlock(&c->lock);
	return result ? -EIO : 0;
}
static void parse_partition(ata_disk *disk, unsigned capacity)
{
	if (ata_volume_count == 4)
		return;
	partition *p = zalloc(sizeof(*p));
	struct fixed_disk *volume = zalloc(sizeof(*volume));
	if (!p || !volume) {
		free(p);
		free(volume);
		return;
	}
	p->disk = disk;
	p->sector_count = capacity;
	ata_volume_count++;
	volume->private = p;
	volume->sectors = capacity;
	volume->sector_size = 512;
	volume->major = HDD_MAJOR;
	volume->index = disk->name[2] - 'a';
	volume->pci = g_ide_pci;
	strcpy(volume->name, disk->name);
	volume->rw = ata_fixed_rw;
	volume->flush = ata_fixed_flush;
	if (fixed_disk_register(volume))
		klog("ata: disk registration failed: %s\n", disk->name);
}

static int ide_probe_pci(uint32_t device, uint16_t vendor, uint16_t id,
			 const pci_device_id *match)
{
	static int initialized;
	unsigned bar4;
	/* Channels use the compatibility-mode ports and IRQs. */
	if (initialized || (pci_read_field(device, PCI_PROG_IF, 1) & 5))
		return -ENODEV;
	initialized = 1;
	g_ide_pci = device;
	bar4 = pci_read_field(device, PCI_BAR4, 4);
	if (bar4 & 1)
		g_bm_base = bar4 & ~3U;
	hdd_init();
	return 0;
}

static const pci_device_id ide_ids[] = {
	{ .vendor_id = PCI_ANY_ID,
	  .device_id = PCI_ANY_ID,
	  .class_code = 0x0101,
	  .class_mask = 0xffff },
};
static driver_t ide_driver = {
	.name = "ata-ide",
	.bus = DEVICE_BUS_PCI,
	.pci_ids = ide_ids,
	.pci_id_count = 1,
	.probe_pci = ide_probe_pci,
};
DRIVER_REGISTER(ide_driver);
