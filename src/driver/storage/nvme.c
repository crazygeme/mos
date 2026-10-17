/* PCIe NVM command set, physically contiguous queues and PRP DMA. */
#include "fixed.h"
#include <device/devnums.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <errno.h>
#define QDEPTH 32
#define DATA_PAGES 32
struct nvme_command {
	uint32_t op, nsid;
	uint64_t reserved, metadata, prp1, prp2;
	uint32_t cdw[6];
};
struct nvme_completion {
	uint32_t result, reserved;
	uint16_t head, sqid, cid, status;
};
_Static_assert(sizeof(struct nvme_command) == 64, "NVMe SQ entry size");
_Static_assert(sizeof(struct nvme_completion) == 16, "NVMe CQ entry size");
struct queue {
	struct nvme_command *sq;
	volatile struct nvme_completion *cq;
	unsigned tail, head, phase, id, depth;
	uint16_t cid;
};
struct nvme {
	volatile uint32_t *regs;
	unsigned pci, stride, timeout, max_bytes, dead;
	mutex_t lock;
	struct queue admin, io;
	void *data;
	uint64_t *prps;
	unsigned number;
};
struct nvme_ns {
	struct fixed_disk disk;
	struct nvme *ctrl;
	unsigned nsid;
};
static unsigned controllers, namespaces;
static uint32_t rd(struct nvme *c, unsigned off)
{
	return c->regs[off / 4];
}
static void wr(struct nvme *c, unsigned off, uint32_t v)
{
	c->regs[off / 4] = v;
}
static void fence(void)
{
	__sync_synchronize();
}
static int ready(struct nvme *c, unsigned want)
{
	uint64_t end = time_deadline_ms(c->timeout);
	do {
		unsigned s = rd(c, 0x1c);
		if ((s & 1) == want)
			return 0;
		if (s & 2)
			return -EIO;
		usleep(100);
	} while (time_now_ms() < end);
	return -ETIMEDOUT;
}
/* A timed-out DMA allocation remains owned until controller reset completes. */
static void fail_stop(struct nvme *c)
{
	c->dead = 1;
	wr(c, 0x14, 0);
	ready(c, 0);
	pci_write_field(c->pci, PCI_COMMAND, 2,
			pci_read_field(c->pci, PCI_COMMAND, 2) & ~4U);
}
static int submit(struct nvme *c, struct queue *q, struct nvme_command *cmd)
{
	if (c->dead)
		return -EIO;
	unsigned cid = ++q->cid;
	cmd->op = (cmd->op & 0xffff) | (cid << 16);
	q->sq[q->tail] = *cmd;
	q->tail = (q->tail + 1) % q->depth;
	fence();
	wr(c, 0x1000 + 2 * q->id * c->stride, q->tail);
	uint64_t start = time_now_us(), end = time_deadline_ms(30000);
	volatile struct nvme_completion *e = &q->cq[q->head];
	while ((e->status & 1) != q->phase) {
		if ((rd(c, 0x1c) & 2) || time_now_ms() >= end) {
			klog("nvme: command timeout/fatal q=%u op=%x\n", q->id,
			     cmd->op & 255);
			fail_stop(c);
			return -EIO;
		}
		if (time_now_us() - start > 100)
			usleep(100);
		else
			__asm__ volatile("pause");
	}
	fence();
	unsigned status = e->status, valid = e->cid == cid && e->sqid == q->id;
	q->head = (q->head + 1) % q->depth;
	if (!q->head)
		q->phase ^= 1;
	fence();
	wr(c, 0x1000 + (2 * q->id + 1) * c->stride, q->head);
	if (!valid) {
		fail_stop(c);
		return -EIO;
	}
	if (status >> 1) {
		klog("nvme: command failed q=%u op=%x status=%x\n", q->id,
		     cmd->op & 255, status);
		return -EIO;
	}
	return 0;
}
static int queue_alloc(struct queue *q, unsigned id, unsigned depth)
{
	q->sq = (void *)vm_alloc_dma(1);
	q->cq = (void *)vm_alloc_dma(1);
	q->id = id;
	q->phase = 1;
	q->depth = depth;
	if (!q->sq || !q->cq)
		return -ENOMEM;
	memset(q->sq, 0, PAGE_SIZE);
	memset((void *)q->cq, 0, PAGE_SIZE);
	return 0;
}
static void data_pointer(struct nvme *c, struct nvme_command *cmd,
			 unsigned bytes)
{
	uint64_t phys = VIRT_TO_PHY(c->data);
	cmd->prp1 = phys;
	if (bytes <= PAGE_SIZE)
		return;
	if (bytes <= 2 * PAGE_SIZE) {
		cmd->prp2 = phys + PAGE_SIZE;
		return;
	}
	unsigned pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	for (unsigned i = 1; i < pages; i++)
		c->prps[i - 1] = phys + i * PAGE_SIZE;
	cmd->prp2 = VIRT_TO_PHY(c->prps);
}
static int identify(struct nvme *c, unsigned nsid, unsigned cns)
{
	struct nvme_command cmd = { .op = 6, .nsid = nsid, .cdw = { cns } };
	memset(c->data, 0, PAGE_SIZE);
	data_pointer(c, &cmd, PAGE_SIZE);
	return submit(c, &c->admin, &cmd);
}
static int ns_rw(struct fixed_disk *d, void *buf, uint64_t lba, unsigned count,
		 int write)
{
	struct nvme_ns *ns = d->private;
	struct nvme *c = ns->ctrl;
	if (lba > d->sectors || count > d->sectors - lba)
		return -EIO;
	mutex_lock(&c->lock);
	int ret = 0;
	while (count && !ret) {
		unsigned n = count, max = c->max_bytes / d->sector_size;
		if (n > max)
			n = max;
		if (n > 65536)
			n = 65536;
		unsigned bytes = n * d->sector_size;
		struct nvme_command cmd = { .op = write ? 1 : 2,
					    .nsid = ns->nsid,
					    .cdw = { (uint32_t)lba, lba >> 32,
						     n - 1 } };
		if (write)
			memcpy(c->data, buf, bytes);
		data_pointer(c, &cmd, bytes);
		ret = submit(c, &c->io, &cmd);
		if (!ret && !write)
			memcpy(buf, c->data, bytes);
		lba += n;
		count -= n;
		buf = (char *)buf + bytes;
	}
	mutex_unlock(&c->lock);
	return ret;
}
static int ns_flush(struct fixed_disk *d)
{
	struct nvme_ns *ns = d->private;
	struct nvme *c = ns->ctrl;
	struct nvme_command cmd = { .op = 0, .nsid = ns->nsid };
	mutex_lock(&c->lock);
	int ret = submit(c, &c->io, &cmd);
	mutex_unlock(&c->lock);
	return ret;
}
static void ns_shutdown(struct fixed_disk *d)
{
	struct nvme *c = ((struct nvme_ns *)d->private)->ctrl;
	mutex_lock(&c->lock);
	if (!c->dead) {
		wr(c, 0x14, (rd(c, 0x14) & ~(3U << 14)) | (1U << 14));
		uint64_t end = time_deadline_ms(5000);
		while ((rd(c, 0x1c) & 12) != 8 && time_now_ms() < end)
			usleep(100);
		fail_stop(c);
	}
	mutex_unlock(&c->lock);
}
static int ns_discard(struct fixed_disk *d, uint64_t lba, uint64_t count)
{
	struct nvme_ns *ns = d->private;
	struct nvme *c = ns->ctrl;
	mutex_lock(&c->lock);
	int ret = 0;
	while (count && !ret) {
		uint32_t n = count > 0xffffffffU ? 0xffffffffU : count;
		struct {
			uint32_t attr, count;
			uint64_t lba;
		} range = { 0, n, lba };
		memcpy(c->data, &range, sizeof(range));
		struct nvme_command cmd = { .op = 9,
					    .nsid = ns->nsid,
					    .cdw = { 0, 4 } };
		data_pointer(c, &cmd, sizeof(range));
		ret = submit(c, &c->io, &cmd);
		lba += n;
		count -= n;
	}
	mutex_unlock(&c->lock);
	return ret;
}
static int nvme_probe(unsigned pci, uint16_t vendor, uint16_t device,
		      const pci_device_id *match)
{
	(void)vendor;
	(void)device;
	(void)match;
	if (pci_read_field(pci, PCI_PROG_IF, 1) != 2 || controllers == 16)
		return -ENODEV;
	struct nvme *c = zalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;
	c->pci = pci;
	c->regs = storage_map_bar(pci, 0, 0x2000);
	if (!c->regs) {
		free(c);
		return -ENODEV;
	}
	uint64_t cap = (uint64_t)rd(c, 4) << 32 | rd(c, 0);
	c->timeout = ((cap >> 24) & 255) * 500;
	if (c->timeout < 500)
		c->timeout = 500;
	c->stride = 4U << ((cap >> 32) & 15);
	c->max_bytes = DATA_PAGES * PAGE_SIZE;
	pci_resource resources[7];
	pci_get_resources(pci, resources);
	if (!(cap & (1ULL << 37)) || ((cap >> 48) & 15) ||
	    0x1000ULL + 4 * c->stride > resources[0].size)
		goto unsupported;
	wr(c, 0xc, ~0U);
	wr(c, 0x14, 0);
	if (ready(c, 0))
		goto failed;
	unsigned depth = (cap & 0xffff) + 1;
	if (depth > QDEPTH)
		depth = QDEPTH;
	if (depth < 2)
		goto unsupported;
	if (queue_alloc(&c->admin, 0, depth) || queue_alloc(&c->io, 1, depth))
		goto failed;
	c->data = (void *)vm_alloc_dma(DATA_PAGES);
	c->prps = (void *)vm_alloc_dma(1);
	if (!c->data || !c->prps)
		goto failed;
	mutex_init(&c->lock);
	c->number = controllers++;
	wr(c, 0x24, (depth - 1) | ((depth - 1) << 16));
	uint64_t pa = VIRT_TO_PHY(c->admin.sq);
	wr(c, 0x28, pa);
	wr(c, 0x2c, pa >> 32);
	pa = VIRT_TO_PHY(c->admin.cq);
	wr(c, 0x30, pa);
	wr(c, 0x34, pa >> 32);
	wr(c, 0x14, 1 | (6 << 16) | (4 << 20));
	if (ready(c, 1))
		goto failed;
	if (identify(c, 0, 1))
		goto failed;
	unsigned char *id = c->data;
	unsigned mdts = id[77];
	uint32_t nn;
	memcpy(&nn, id + 516, 4);
	uint16_t oncs;
	memcpy(&oncs, id + 520, 2);
	if (mdts && mdts < 20 && c->max_bytes > (4096U << mdts))
		c->max_bytes = 4096U << mdts;
	struct nvme_command cmd = { .op = 9, .cdw = { 7, 0 } };
	if (submit(c, &c->admin, &cmd))
		goto failed;
	cmd = (struct nvme_command){ .op = 5,
				     .prp1 = VIRT_TO_PHY(c->io.cq),
				     .cdw = { 1 | ((depth - 1) << 16), 1 } };
	if (submit(c, &c->admin, &cmd))
		goto failed;
	cmd = (struct nvme_command){ .op = 1,
				     .prp1 = VIRT_TO_PHY(c->io.sq),
				     .cdw = { 1 | ((depth - 1) << 16),
					      1 | (1 << 16) } };
	if (submit(c, &c->admin, &cmd))
		goto failed;
	/* Active namespace lists preserve sparse namespace identifiers. */
	uint32_t last = 0;
	unsigned found = 0;
	while (found < 16 && namespaces < 16) {
		uint32_t ids[1024];
		if (identify(c, last, 2)) {
			if (last)
				break;
			unsigned limit = nn < 16 ? nn : 16;
			memset(ids, 0, sizeof(ids));
			for (unsigned i = 0; i < limit; i++)
				ids[i] = i + 1;
		} else
			memcpy(ids, c->data, sizeof(ids));
		if (!ids[0])
			break;
		unsigned progressed = 0;
		for (unsigned i = 0;
		     i < 1024 && ids[i] && found < 16 && namespaces < 16; i++) {
			if (ids[i] <= last)
				break;
			last = ids[i];
			progressed = 1;
			if (identify(c, last, 0))
				continue;
			id = c->data;
			uint64_t sectors;
			memcpy(&sectors, id, 8);
			unsigned format = (id[26] & 15) | ((id[26] >> 5) << 4);
			uint16_t metadata;
			memcpy(&metadata, id + 128 + format * 4, 2);
			unsigned shift = id[130 + format * 4];
			if (!sectors || format > id[25] || metadata ||
			    (id[29] & 7) || shift < 9 || shift > 12 ||
			    (1U << shift) > c->max_bytes) {
				klog("nvme: namespace %u has unsupported LBA format\n",
				     last);
				continue;
			}
			struct nvme_ns *ns = zalloc(sizeof(*ns));
			if (!ns)
				break;
			ns->ctrl = c;
			ns->nsid = last;
			struct fixed_disk *d = &ns->disk;
			d->private = ns;
			d->sectors = sectors;
			d->sector_size = 1U << shift;
			d->major = NVME_MAJOR;
			d->index = namespaces++;
			d->pci = pci;
			sprintf(d->name, "nvme%un%u", c->number, last);
			d->rw = ns_rw;
			d->flush = ns_flush;
			d->shutdown = ns_shutdown;
			if (oncs & 4)
				d->discard = ns_discard;
			if (fixed_disk_register(d)) {
				free(ns);
				continue;
			}
			found++;
		}
		if (!progressed || last >= nn)
			break;
	}
	printk("nvme: controller %u namespaces=%u transfer=%u\n", c->number,
	       found, c->max_bytes);
	return found ? 0 : -ENODEV;
unsupported:
	fail_stop(c);
	free(c);
	return -ENODEV;
failed:
	fail_stop(c);
	if (c->admin.sq)
		vm_free((vaddr_t)c->admin.sq, 1);
	if (c->admin.cq)
		vm_free((vaddr_t)c->admin.cq, 1);
	if (c->io.sq)
		vm_free((vaddr_t)c->io.sq, 1);
	if (c->io.cq)
		vm_free((vaddr_t)c->io.cq, 1);
	if (c->data)
		vm_free((vaddr_t)c->data, DATA_PAGES);
	if (c->prps)
		vm_free((vaddr_t)c->prps, 1);
	free(c);
	return -EIO;
}
static const pci_device_id nvme_ids[] = { { .vendor_id = PCI_ANY_ID,
					    .device_id = PCI_ANY_ID,
					    .class_code = 0x0108,
					    .class_mask = 0xffff } };
static driver_t nvme_driver = { .name = "nvme",
				.bus = DEVICE_BUS_PCI,
				.pci_ids = nvme_ids,
				.pci_id_count = 1,
				.probe_pci = nvme_probe };
DRIVER_REGISTER(nvme_driver);
