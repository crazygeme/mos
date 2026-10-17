/* SATA disk and SSD transport using one serialized AHCI command slot. */
#include "fixed.h"
#include <device/devnums.h>
#include <lib/klib.h>
#include <errno.h>
#define SATA_PAGES 32
struct ahci_port {
	struct fixed_disk disk;
	volatile uint32_t *regs;
	void *list, *fis, *table, *data;
	unsigned dead, lba48, trim, flush_ext;
};
struct command_header {
	uint16_t flags, prdtl;
	uint32_t transferred, table, table_hi, reserved[4];
};
struct prdt {
	uint32_t base, high, reserved, bytes;
};
_Static_assert(sizeof(struct command_header) == 32, "AHCI command header size");
static unsigned disk_number;
static int wait_bits(volatile uint32_t *reg, unsigned mask, unsigned value,
		     unsigned ms)
{
	uint64_t end = time_deadline_ms(ms);
	do {
		if ((*reg & mask) == value)
			return 0;
		usleep(100);
	} while (time_now_ms() < end);
	return -ETIMEDOUT;
}
static int stop(struct ahci_port *p)
{
	p->regs[0x18 / 4] &= ~1U;
	if (wait_bits(p->regs + 0x18 / 4, 1 << 15, 0, 1000))
		return -EIO;
	p->regs[0x18 / 4] &= ~(1U << 4);
	return wait_bits(p->regs + 0x18 / 4, 1 << 14, 0, 1000);
}
static int command(struct ahci_port *p, unsigned op, uint64_t lba,
		   unsigned sectors, unsigned bytes, int write, int extended)
{
	if (p->dead)
		return -EIO;
	if (wait_bits(p->regs + 0x20 / 4, 0x88, 0, 5000)) {
		p->dead = 1;
		stop(p);
		return -EIO;
	}
	struct command_header *h = p->list;
	memset(h, 0, sizeof(*h));
	memset(p->table, 0, 256);
	h->flags = 5 | (write ? 1 << 6 : 0);
	h->prdtl = bytes ? 1 : 0;
	uint64_t pa = VIRT_TO_PHY(p->table);
	h->table = pa;
	h->table_hi = pa >> 32;
	unsigned char *f = p->table;
	f[0] = 0x27;
	f[1] = 0x80;
	f[2] = op;
	f[4] = lba;
	f[5] = lba >> 8;
	f[6] = lba >> 16;
	f[7] = 0x40;
	if (extended) {
		f[8] = lba >> 24;
		f[9] = lba >> 32;
		f[10] = lba >> 40;
		f[13] = sectors >> 8;
	} else
		f[7] |= (lba >> 24) & 15;
	f[12] = sectors;
	if (op == 0x06)
		f[3] = 1; /* ATA DATA SET MANAGEMENT: TRIM. */
	if (bytes) {
		struct prdt *pr = (void *)(f + 128);
		pa = VIRT_TO_PHY(p->data);
		pr->base = pa;
		pr->high = pa >> 32;
		pr->bytes = bytes - 1;
	}
	p->regs[0x10 / 4] = ~0U;
	__sync_synchronize();
	p->regs[0x38 / 4] = 1;
	uint64_t start = time_now_us(), end = time_deadline_ms(30000);
	while (p->regs[0x38 / 4] & 1) {
		if ((p->regs[0x10 / 4] & ((1U << 30) | (1U << 29) | (1U << 28) |
					  (1U << 27) | (1U << 24))) ||
		    time_now_ms() >= end) {
			klog("ahci: command failed op=%x status=%x tfd=%x\n",
			     op, p->regs[0x10 / 4], p->regs[0x20 / 4]);
			p->dead = 1;
			stop(p);
			return -EIO;
		}
		if (time_now_us() - start > 100)
			usleep(100);
		else
			__asm__ volatile("pause");
	}
	__sync_synchronize();
	if (p->regs[0x20 / 4] & 0x21)
		return -EIO;
	if (bytes && h->transferred != bytes)
		return -EIO;
	return 0;
}
static int sata_rw(struct fixed_disk *d, void *buf, uint64_t lba,
		   unsigned count, int write)
{
	struct ahci_port *p = d->private;
	if (lba > d->sectors || count > d->sectors - lba)
		return -EIO;
	int ret = 0;
	while (count && !ret) {
		unsigned n = count,
			 max = SATA_PAGES * PAGE_SIZE / d->sector_size;
		if (!p->lba48 && max > 256)
			max = 256;
		if (n > max)
			n = max;
		unsigned bytes = n * d->sector_size;
		if (write)
			memcpy(p->data, buf, bytes);
		ret = command(p,
			      p->lba48 ? (write ? 0x35 : 0x25) :
					 (write ? 0xca : 0xc8),
			      lba, n, bytes, write, p->lba48);
		if (!ret && !write)
			memcpy(buf, p->data, bytes);
		lba += n;
		count -= n;
		buf = (char *)buf + bytes;
	}
	return ret;
}
static int sata_flush(struct fixed_disk *d)
{
	struct ahci_port *p = d->private;
	return command(p, p->flush_ext ? 0xea : 0xe7, 0, 0, 0, 0, p->flush_ext);
}
static void sata_shutdown(struct fixed_disk *d)
{
	struct ahci_port *p = d->private;
	stop(p);
	p->dead = 1;
}
static int sata_discard(struct fixed_disk *d, uint64_t lba, uint64_t count)
{
	struct ahci_port *p = d->private;
	while (count) {
		uint64_t *ranges = p->data;
		memset(ranges, 0, 512);
		for (unsigned i = 0; i < 64 && count; i++) {
			unsigned n = count > 65535 ? 65535 : count;
			ranges[i] = lba | ((uint64_t)n << 48);
			lba += n;
			count -= n;
		}
		int ret = command(p, 0x06, 0, 1, 512, 1, 1);
		if (ret)
			return ret;
	}
	return 0;
}
static int port_init(unsigned pci, volatile uint32_t *regs)
{
	struct ahci_port *p = zalloc(sizeof(*p));
	if (!p)
		return -ENOMEM;
	p->regs = regs;
	if (stop(p)) {
		free(p);
		return -EIO;
	}
	p->list = (void *)vm_alloc_dma(1);
	p->fis = (void *)vm_alloc_dma(1);
	p->table = (void *)vm_alloc_dma(1);
	p->data = (void *)vm_alloc_dma(SATA_PAGES);
	if (!p->list || !p->fis || !p->table || !p->data)
		goto release;
	memset(p->list, 0, PAGE_SIZE);
	memset(p->fis, 0, PAGE_SIZE);
	uint64_t pa = VIRT_TO_PHY(p->list);
	regs[0] = pa;
	regs[1] = pa >> 32;
	pa = VIRT_TO_PHY(p->fis);
	regs[2] = pa;
	regs[3] = pa >> 32;
	regs[0x14 / 4] = 0;
	regs[0x30 / 4] = ~0U;
	regs[0x10 / 4] = ~0U;
	regs[0x18 / 4] |= (1 << 4) | (1 << 1) | (1 << 2);
	if (wait_bits(regs + 0x20 / 4, 0x88, 0, 5000) ||
	    regs[0x24 / 4] != 0x101)
		goto release;
	regs[0x18 / 4] |= 1;
	if (command(p, 0xec, 0, 0, 512, 0, 0))
		goto release;
	uint16_t *id = p->data;
	if (!(id[49] & (1 << 9)) || !(id[49] & (1 << 8)))
		goto release;
	p->lba48 = (id[83] & 0xc400) == 0x4400;
	p->flush_ext = p->lba48 && (id[83] & (1 << 13));
	p->trim = p->lba48 && (id[169] & 1);
	struct fixed_disk *d = &p->disk;
	d->sector_size = 512;
	if ((id[106] & 0xd000) == 0x5000) {
		unsigned words = (unsigned)id[117] | ((unsigned)id[118] << 16);
		if (words < 256 || words > 2048)
			goto release;
		d->sector_size = words * 2;
	}
	if (p->lba48)
		memcpy(&d->sectors, id + 100, 8);
	else
		d->sectors = (unsigned)id[60] | ((unsigned)id[61] << 16);
	if (!d->sectors ||
	    d->sectors > (p->lba48 ? (1ULL << 48) : (1ULL << 28)))
		goto release;
	d->index = disk_number++;
	d->major = SATA_MAJOR;
	d->pci = pci;
	d->private = p;
	sprintf(d->name, "sata%u", d->index);
	d->rw = sata_rw;
	d->flush = sata_flush;
	d->shutdown = sata_shutdown;
	if (p->trim)
		d->discard = sata_discard;
	if (!fixed_disk_register(d))
		return 0;
release:
	/* Retain DMA buffers if command/FIS engines cannot be quiesced. */
	if (stop(p)) {
		p->dead = 1;
		return -EIO;
	}
	regs[0] = 0;
	regs[1] = 0;
	regs[2] = 0;
	regs[3] = 0;
	if (p->list)
		vm_free((vaddr_t)p->list, 1);
	if (p->fis)
		vm_free((vaddr_t)p->fis, 1);
	if (p->table)
		vm_free((vaddr_t)p->table, 1);
	if (p->data)
		vm_free((vaddr_t)p->data, SATA_PAGES);
	free(p);
	return -EIO;
}
static int ahci_probe(unsigned pci, uint16_t vendor, uint16_t device,
		      const pci_device_id *match)
{
	(void)vendor;
	(void)device;
	(void)match;
	if (pci_read_field(pci, PCI_PROG_IF, 1) != 1)
		return -ENODEV;
	volatile uint32_t *r = storage_map_bar(pci, 5, 0x100);
	if (!r)
		return -ENODEV;
	/* BIOS/OS handoff is mandatory when CAP2.BOH is set. */
	if (r[0x24 / 4] & 1) {
		r[0x28 / 4] |= 2;
		if (wait_bits(r + 0x28 / 4, 0x11, 0, 25000))
			return -ETIMEDOUT;
	}
	r[1] = (r[1] | (1U << 31)) & ~2U;
	r[1] |= 1;
	if (wait_bits(r + 1, 1, 0, 1000))
		return -EIO;
	r[1] = 1U << 31;
	pci_resource resources[7];
	pci_get_resources(pci, resources);
	unsigned pi = r[3];
	for (unsigned i = 0; i < 32; i++)
		if ((pi & (1U << i)) &&
		    0x100 + (i + 1) * 0x80 <= resources[5].size) {
			volatile uint32_t *port = r + (0x100 + i * 0x80) / 4;
			if ((port[0x28 / 4] & 15) != 3 ||
			    ((port[0x28 / 4] >> 8) & 15) != 1)
				continue;
			if (disk_number == 16)
				break;
			int ret = port_init(pci, port);
			if (ret)
				klog("ahci: port %u initialization failed: %d\n",
				     i, ret);
		}
	return 0;
}
static const pci_device_id ahci_ids[] = { { .vendor_id = PCI_ANY_ID,
					    .device_id = PCI_ANY_ID,
					    .class_code = 0x0106,
					    .class_mask = 0xffff } };
static driver_t ahci_driver = { .name = "ahci",
				.bus = DEVICE_BUS_PCI,
				.pci_ids = ahci_ids,
				.pci_id_count = 1,
				.probe_pci = ahci_probe };
DRIVER_REGISTER(ahci_driver);
