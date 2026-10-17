#include "fixed.h"
#include <device/blockdev.h>
#include <device/devnums.h>
#include <fs/entries.h>
#include <fs/sysfs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <lib/klib.h>
#include <errno.h>
#include <mm/mmu.h>
#include <mm/phymm.h>
#define FIXED_PARTS 16
#define FIXED_DISKS 16
#define INT64_MAX_VALUE 0x7fffffffffffffffLL
struct partition {
	struct fixed_disk *disk;
	uint64_t start, sectors;
	unsigned minor;
	char name[32];
};
static struct partition parts[FIXED_DISKS][FIXED_PARTS];
static unsigned disk_count;
static file *storage_open(super_block *, unsigned, int);
#define CACHE_BYTES PAGE_SIZE
struct cache_line {
	uint64_t sector;
	paddr_t physical;
	unsigned dirty;
	struct rb_node node;
	list_entry lru;
};
struct fixed_cache {
	struct rb_root lines;
	list_entry lru;
	unsigned pages, peak_pages;
	uint64_t read_bytes, write_bytes, physical_read, physical_write;
	unsigned hits, searches;
};
static unsigned cache_pages;
extern unsigned fs_page_cache_pages;

static int cache_store(struct fixed_disk *d, struct cache_line *line)
{
	if (!line->dirty)
		return 0;
	unsigned n = CACHE_BYTES / d->sector_size;
	if (n > d->sectors - line->sector)
		n = d->sectors - line->sector;
	if (mm_kmap_phys(line->physical) != 1)
		return -ENOMEM;
	int ret = d->rw(d, (void *)PHY_TO_VIRT(line->physical), line->sector, n,
			1);
	mm_kunmap_phys(line->physical);
	if (!ret) {
		line->dirty = 0;
		d->cache->physical_write += (uint64_t)n * d->sector_size;
	}
	return ret;
}
static int cache_flush(struct fixed_disk *d)
{
	for (list_entry *e = d->cache->lru.next; e != &d->cache->lru;
	     e = e->next) {
		int ret =
			cache_store(d, container_of(e, struct cache_line, lru));
		if (ret)
			return ret;
	}
	return d->flush(d);
}
static void cache_remove(struct fixed_disk *d, struct cache_line *line)
{
	rb_erase(&line->node, &d->cache->lines);
	list_remove_entry(&line->lru);
	d->cache->pages--;
	__sync_fetch_and_sub(&cache_pages, 1);
}
static void cache_release(struct cache_line *line)
{
	phymm_free_user(PHY_TO_PAGE_IDX(line->physical));
	free(line);
}
static int cache_rw(struct fixed_disk *d, void *buffer, uint64_t sector,
		    unsigned count, int write, unsigned budget)
{
	struct fixed_cache *cache = d->cache;
	unsigned span = CACHE_BYTES / d->sector_size;
	if (sector > d->sectors || count > d->sectors - sector)
		return -EIO;
	while (count) {
		uint64_t head = sector - sector % span;
		unsigned off = sector - head, n = span - off;
		if (n > count)
			n = count;
		struct cache_line *line = NULL;
		struct rb_node *node = cache->lines.rb_node;
		cache->searches++;
		while (node) {
			struct cache_line *cur =
				rb_entry(node, struct cache_line, node);
			if (head == cur->sector) {
				line = cur;
				cache->hits++;
				break;
			}
			node = head < cur->sector ? node->rb_left :
						    node->rb_right;
		}
		if (!line) {
			unsigned page = PHYMM_INVALID;
			if (cache_pages < budget)
				page = phymm_alloc_cache();
			/* Keep one line available when only low RAM remains. */
			if (page == PHYMM_INVALID && list_is_empty(&cache->lru))
				page = phymm_alloc_user();
			if (page != PHYMM_INVALID) {
				line = zalloc(sizeof(*line));
				if (!line) {
					phymm_free_user(page);
					return -ENOMEM;
				}
				line->physical = (paddr_t)page * PAGE_SIZE;
			} else if (!list_is_empty(&cache->lru)) {
				line = container_of(cache->lru.next,
						    struct cache_line, lru);
				int ret = cache_store(d, line);
				if (ret)
					return ret;
				cache_remove(d, line);
			} else {
				return -ENOMEM;
			}
			line->sector = head;
			line->dirty = 0;
			unsigned size = span;
			if (size > d->sectors - head)
				size = d->sectors - head;
			if (mm_kmap_phys(line->physical) != 1) {
				cache_release(line);
				return -ENOMEM;
			}
			int ret = 0;
			if (!write || off || n < size) {
				ret = d->rw(d,
					    (void *)PHY_TO_VIRT(line->physical),
					    head, size, 0);
				if (!ret)
					cache->physical_read +=
						(uint64_t)size * d->sector_size;
			}
			mm_kunmap_phys(line->physical);
			if (ret) {
				cache_release(line);
				return ret;
			}
			struct rb_node **link = &cache->lines.rb_node,
				       *parent = NULL;
			while (*link) {
				struct cache_line *cur = rb_entry(
					*link, struct cache_line, node);
				parent = *link;
				link = head < cur->sector ? &parent->rb_left :
							    &parent->rb_right;
			}
			rb_init_node(&line->node);
			rb_link_node(&line->node, parent, link);
			rb_insert_color(&line->node, &cache->lines);
			list_init(&line->lru);
			list_insert_tail(&cache->lru, &line->lru);
			cache->pages++;
			__sync_fetch_and_add(&cache_pages, 1);
			if (cache->pages > cache->peak_pages)
				cache->peak_pages = cache->pages;
		}
		if (mm_kmap_phys(line->physical) != 1)
			return -ENOMEM;
		void *data = (void *)(PHY_TO_VIRT(line->physical) +
				      off * d->sector_size);
		unsigned bytes = n * d->sector_size;
		if (write) {
			memcpy(data, buffer, bytes);
			line->dirty = 1;
			cache->write_bytes += bytes;
		} else {
			memcpy(buffer, data, bytes);
			cache->read_bytes += bytes;
		}
		mm_kunmap_phys(line->physical);
		list_remove_entry(&line->lru);
		list_insert_tail(&cache->lru, &line->lru);
		count -= n;
		sector += n;
		buffer = (char *)buffer + bytes;
	}
	return 0;
}
static int disk_rw(struct partition *p, void *b, uint64_t s, unsigned n, int w)
{
	if (s > p->sectors || n > p->sectors - s)
		return -EIO;
	phymm_cache_policy policy;
	phymm_get_cache_policy_snapshot(fs_page_cache_pages, cache_pages,
					&policy);
	rmutex_lock(&p->disk->lock);
	int ret = cache_rw(p->disk, b, s + p->start, n, w, policy.block_pages);
	rmutex_unlock(&p->disk->lock);
	return ret;
}
static int storage_read(void *p, void *b, uint64_t s, unsigned n)
{
	return disk_rw(p, b, s, n, 0);
}
static int storage_write(void *p, const void *b, uint64_t s, unsigned n)
{
	return disk_rw(p, (void *)b, s, n, 1);
}
static const blockdev_io storage_ops = { .read = storage_read,
					 .write = storage_write };
static ssize_t file_rw(file *fp, void *buffer, size_t size, loff_t *pos,
		       int write)
{
	struct partition *p = fp->f_inode->i_private;
	uint64_t length = p->sectors * p->disk->sector_size;
	if (*pos < 0)
		return -EINVAL;
	if ((uint64_t)*pos >= length)
		return write && size ? -ENOSPC : 0;
	if (size > length - *pos)
		size = length - *pos;
	unsigned char *scratch = malloc(p->disk->sector_size);
	if (!scratch)
		return -ENOMEM;
	size_t done = 0;
	int ret = 0;
	/* Serialize partial-sector read/modify/write across all opens. */
	rmutex_lock(&p->disk->lock);
	while (done < size) {
		uint64_t offset = *pos + done;
		unsigned within = offset % p->disk->sector_size,
			 chunk = size - done;
		if (!within && chunk >= p->disk->sector_size) {
			unsigned sectors = chunk / p->disk->sector_size;
			if (sectors > 2048)
				sectors = 2048;
			chunk = sectors * p->disk->sector_size;
			ret = disk_rw(p, (char *)buffer + done,
				      offset / p->disk->sector_size, sectors,
				      write);
		} else {
			if (chunk > p->disk->sector_size - within)
				chunk = p->disk->sector_size - within;
			ret = disk_rw(p, scratch, offset / p->disk->sector_size,
				      1, 0);
			if (!ret) {
				if (write) {
					memcpy(scratch + within,
					       (char *)buffer + done, chunk);
					ret = disk_rw(
						p, scratch,
						offset / p->disk->sector_size,
						1, 1);
				} else
					memcpy((char *)buffer + done,
					       scratch + within, chunk);
			}
		}
		if (ret)
			break;
		done += chunk;
	}
	rmutex_unlock(&p->disk->lock);
	free(scratch);
	*pos += done;
	return done ? (ssize_t)done : ret;
}
static ssize_t node_read(file *f, void *b, size_t n, loff_t *p)
{
	return file_rw(f, b, n, p, 0);
}
static ssize_t node_write(file *f, const void *b, size_t n, loff_t *p)
{
	return file_rw(f, (void *)b, n, p, 1);
}
static int node_ioctl(file *f, unsigned command, void *buffer)
{
	struct partition *p = f->f_inode->i_private;
	if (!buffer && command != BLKFLSBUF)
		return -EINVAL;
	switch (command) {
	case BLKGETSIZE:
		*(unsigned long *)buffer =
			p->sectors * (p->disk->sector_size / 512);
		return 0;
	case BLKGETSIZE64:
		*(uint64_t *)buffer = p->sectors * p->disk->sector_size;
		return 0;
	case BLKSSZGET:
		*(int *)buffer = p->disk->sector_size;
		return 0;
	case BLKFLSBUF: {
		rmutex_lock(&p->disk->lock);
		int ret = cache_flush(p->disk);
		rmutex_unlock(&p->disk->lock);
		return ret;
	}
	case BLKDISCARD: {
		uint64_t *range = buffer;
		unsigned ss = p->disk->sector_size;
		if (!p->disk->discard)
			return -EOPNOTSUPP;
		if (range[0] % ss || range[1] % ss ||
		    range[0] / ss > p->sectors ||
		    range[1] / ss > p->sectors - range[0] / ss)
			return -EINVAL;
		rmutex_lock(&p->disk->lock);
		int ret = cache_flush(p->disk);
		if (!ret)
			ret = p->disk->discard(p->disk,
					       p->start + range[0] / ss,
					       range[1] / ss);
		if (!ret)
			while (!list_is_empty(&p->disk->cache->lru)) {
				struct cache_line *line =
					container_of(p->disk->cache->lru.next,
						     struct cache_line, lru);
				cache_remove(p->disk, line);
				cache_release(line);
			}
		rmutex_unlock(&p->disk->lock);
		return ret;
	}
	default:
		return -ENOTTY;
	}
}
static int node_getattr(file *f, struct stat *s)
{
	struct partition *p = f->f_inode->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFBLK | 0660;
	s->st_rdev = MKDEV(p->disk->major, p->minor);
	s->st_size = p->sectors * p->disk->sector_size;
	s->st_blksize = p->disk->sector_size;
	s->st_blocks = p->sectors * (p->disk->sector_size / 512);
	s->st_nlink = 1;
	return 0;
}
static loff_t node_seek(file *f, loff_t offset, int whence)
{
	struct partition *p = f->f_inode->i_private;
	loff_t base = whence == SEEK_SET ? 0 :
		      whence == SEEK_CUR ? f->f_pos :
		      whence == SEEK_END ? p->sectors * p->disk->sector_size :
					   -1;
	if (base < 0 || (offset > 0 && base > INT64_MAX_VALUE - offset) ||
	    offset < -base)
		return -EINVAL;
	f->f_pos = base + offset;
	return f->f_pos;
}
static int node_release(file *f)
{
	free(f->f_inode);
	free(f);
	return 0;
}
static const file_operations storage_fops = { .read = node_read,
					      .write = node_write,
					      .ioctl = node_ioctl,
					      .getattr = node_getattr,
					      .llseek = node_seek,
					      .release = node_release };

static file *storage_open(super_block *sb, unsigned rdev, int flags)
{
	(void)sb;
	struct partition *p = NULL;
	for (unsigned i = 0; i < disk_count; i++)
		for (unsigned j = 0; j < FIXED_PARTS; j++)
			if (parts[i][j].sectors &&
			    parts[i][j].disk->major == MAJOR(rdev) &&
			    parts[i][j].minor == MINOR(rdev))
				p = &parts[i][j];
	if (!p)
		return NULL;
	file *f = zalloc(sizeof(*f));
	inode *node = zalloc(sizeof(*node));
	if (!f || !node) {
		free(f);
		free(node);
		return NULL;
	}
	node->i_mode = S_IFBLK | 0660;
	node->i_private = p;
	f->f_inode = node;
	f->f_count = 1;
	f->f_fop = &storage_fops;
	f->f_mode = flags & O_ACCMODE;
	f->f_flag = flags;
	return f;
}
static void publish(struct fixed_disk *d, unsigned slot, unsigned nr,
		    uint64_t start, uint64_t count)
{
	if (!count || start >= d->sectors || count > d->sectors - start ||
	    nr >= FIXED_PARTS)
		return;
	struct partition *p = &parts[slot][nr];
	p->disk = d;
	p->start = start;
	p->sectors = count;
	p->minor = d->index * FIXED_PARTS + nr;
	if (nr)
		sprintf(p->name, "%s%s%u", d->name,
			d->major == HDD_MAJOR ? "" : "p", nr);
	else
		strcpy(p->name, d->name);
	blockdev_register(p->name, d->major, p->minor, count * d->sector_size,
			  nr ? BLOCKDEV_FLAG_VOLUME : 0);
	if (blockdev_bind_io(p->name, d->sector_size, &storage_ops, p)) {
		blockdev_unregister(p->name);
		p->sectors = 0;
		return;
	}
	vfs_entry_device(devfs_entries(), p->name, S_IFBLK | 0660,
			 MKDEV(d->major, p->minor),
			 d->major == NVME_MAJOR ? "nvme" : "sata",
			 storage_open);
	vfs_entry_node *parent = vfs_entry_directory(
		driver_device_entry(DEVICE_BUS_PCI, d->pci), "block");
	if (nr)
		parent = vfs_entry_child(parent, d->name);
	vfs_entry_node *node = vfs_entry_directory(parent, p->name);
	char text[128];
	sprintf(text, "%u:%u\n", d->major, p->minor);
	vfs_entry_text(node, "dev", text);
	sprintf(text, "%llu\n", count * (d->sector_size / 512));
	vfs_entry_text(node, "size", text);
	vfs_entry_text(node, "removable", "0\n");
	vfs_entry_text(node, "ro", "0\n");
	if (nr) {
		sprintf(text, "%u\n", nr);
		vfs_entry_text(node, "partition", text);
		sprintf(text, "%llu\n", start);
		vfs_entry_text(node, "start", text);
	}
	sprintf(text, "MAJOR=%u\nMINOR=%u\nDEVNAME=%s\nDEVTYPE=%s\n", d->major,
		p->minor, p->name, nr ? "partition" : "disk");
	vfs_entry_text(node, "uevent", text);
	vfs_entry_node *root = vfs_entry_root(device_sysfs_entries());
	vfs_entry_link_ref(vfs_entry_directory_path(root, "class/block", 0755),
			   p->name, node);
	sprintf(text, "%u:%u", d->major, p->minor);
	vfs_entry_link_ref(vfs_entry_directory_path(root, "dev/block", 0755),
			   text, node);
	if (!nr)
		vfs_entry_link_ref(vfs_entry_directory_path(root, "block",
							    0755),
				   p->name, node);
	printk("storage: %s sectors=%llu logical=%u\n", p->name, count,
	       d->sector_size);
}
struct mbr_entry {
	uint8_t boot, chs[3], type, end[3];
	uint32_t start, count;
} __attribute__((packed));
static int extended(unsigned type)
{
	return type == 5 || type == 15 || type == 0x85;
}
static void scan_mbr(struct fixed_disk *d, unsigned slot, unsigned char *buf)
{
	struct mbr_entry entry[4];
	memcpy(entry, buf + 446, sizeof(entry));
	unsigned logical = 5;
	for (unsigned i = 0; i < 4; i++) {
		if (!entry[i].type || !entry[i].count)
			continue;
		if (!extended(entry[i].type)) {
			if (entry[i].type != 0xee)
				publish(d, slot, i + 1, entry[i].start,
					entry[i].count);
			continue;
		}
		uint64_t base = entry[i].start, ebr = base,
			 end = base + entry[i].count;
		if (end > d->sectors)
			continue;
		while (logical < FIXED_PARTS && ebr >= base && ebr < end) {
			if (d->rw(d, buf, ebr, 1, 0) || buf[510] != 0x55 ||
			    buf[511] != 0xaa)
				break;
			struct mbr_entry e[2];
			memcpy(e, buf + 446, sizeof(e));
			if (e[0].type && !extended(e[0].type) && e[0].start &&
			    e[0].count <= end - ebr &&
			    e[0].start <= end - ebr - e[0].count)
				publish(d, slot, logical++, ebr + e[0].start,
					e[0].count);
			uint64_t next = base + e[1].start;
			if (!extended(e[1].type) || next <= ebr)
				break;
			ebr = next;
		}
	}
}
static uint32_t crc32(const void *data, unsigned n)
{
	const unsigned char *b = data;
	uint32_t crc = ~0U;
	while (n--) {
		crc ^= *b++;
		for (unsigned i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
	}
	return ~crc;
}
static void scan_gpt(struct fixed_disk *d, unsigned slot, unsigned char *buf)
{
	if (d->rw(d, buf, 1, 1, 0) || memcmp(buf, "EFI PART", 8))
		return;
	uint32_t size, check, n, es, table_crc;
	uint64_t table, first, last;
	memcpy(&size, buf + 12, 4);
	memcpy(&check, buf + 16, 4);
	if (size < 92 || size > d->sector_size)
		return;
	memset(buf + 16, 0, 4);
	if (crc32(buf, size) != check)
		return;
	memcpy(&first, buf + 40, 8);
	memcpy(&last, buf + 48, 8);
	memcpy(&table, buf + 72, 8);
	memcpy(&n, buf + 80, 4);
	memcpy(&es, buf + 84, 4);
	memcpy(&table_crc, buf + 88, 4);
	if (first > last || last >= d->sectors || es < 128 ||
	    es > d->sector_size || (es & (es - 1)) || !n || n > 4096)
		return;
	uint64_t bytes = (uint64_t)n * es,
		 blocks = (bytes + d->sector_size - 1) / d->sector_size;
	if (table >= d->sectors || blocks > d->sectors - table ||
	    bytes > 1024 * 1024)
		return;
	unsigned char *entries = malloc(blocks * d->sector_size);
	if (!entries)
		return;
	if (!d->rw(d, entries, table, blocks, 0) &&
	    crc32(entries, bytes) == table_crc)
		for (unsigned i = 0; i < n && i + 1 < FIXED_PARTS; i++) {
			unsigned char *e = entries + i * es;
			unsigned used = 0;
			for (unsigned j = 0; j < 16; j++)
				used |= e[j];
			uint64_t a, b;
			memcpy(&a, e + 32, 8);
			memcpy(&b, e + 40, 8);
			if (used && a >= first && b <= last && b >= a)
				publish(d, slot, i + 1, a, b - a + 1);
		}
	free(entries);
}
static void flush_all(void)
{
	for (unsigned i = 0; i < disk_count; i++) {
		struct fixed_disk *d = parts[i][0].disk;
		rmutex_lock(&d->lock);
		if (cache_flush(d))
			klog("storage: flush failed: %s\n", d->name);
		rmutex_unlock(&d->lock);
	}
}
static void close_all(void)
{
	flush_all();
	for (unsigned i = 0; i < disk_count; i++) {
		struct fixed_disk *d = parts[i][0].disk;
		rmutex_lock(&d->lock);
		if (d->shutdown)
			d->shutdown(d);
		rmutex_unlock(&d->lock);
	}
}
static void cache_stats(blockdev_stats *stats)
{
	for (unsigned i = 0; i < disk_count; i++) {
		struct fixed_disk *d = parts[i][0].disk;
		struct fixed_cache *cache = d->cache;
		rmutex_lock(&d->lock);
		stats->cached_bytes += (uint64_t)cache->pages * CACHE_BYTES;
		stats->peak_bytes += (uint64_t)cache->peak_pages * CACHE_BYTES;
		stats->read_bytes += cache->read_bytes;
		stats->write_bytes += cache->write_bytes;
		stats->physical_read_bytes += cache->physical_read;
		stats->physical_write_bytes += cache->physical_write;
		stats->hits += cache->hits;
		stats->searches += cache->searches;
		rmutex_unlock(&d->lock);
	}
}
/* Reclaim only clean lines: allocation must not initiate recursive disk I/O. */
static unsigned cache_reclaim(unsigned pages)
{
	unsigned freed = 0;
	for (unsigned i = 0; i < disk_count && freed < pages; i++) {
		struct fixed_disk *d = parts[i][0].disk;
		rmutex_lock(&d->lock);
		list_entry *e = d->cache->lru.next;
		while (e != &d->cache->lru && freed < pages) {
			struct cache_line *line =
				container_of(e, struct cache_line, lru);
			e = e->next;
			if (line->dirty)
				continue;
			cache_remove(d, line);
			cache_release(line);
			freed++;
		}
		rmutex_unlock(&d->lock);
	}
	return freed;
}
static const blockdev_backend backend = { .flush = flush_all,
					  .close = close_all,
					  .reclaim = cache_reclaim,
					  .stats = cache_stats };
int fixed_disk_register(struct fixed_disk *d)
{
	if (disk_count == FIXED_DISKS || d->index >= 16 || !d->sectors ||
	    d->sectors > INT64_MAX_VALUE / d->sector_size)
		return -ENOSPC;
	unsigned char *buf = malloc(d->sector_size);
	if (!buf)
		return -ENOMEM;
	if (!disk_count)
		blockdev_register_backend(&backend);
	d->cache = zalloc(sizeof(*d->cache));
	if (!d->cache) {
		free(buf);
		return -ENOMEM;
	}
	d->cache->lines = (struct rb_root)_RBTREE_ROOT_INIT;
	list_init(&d->cache->lru);
	unsigned slot = disk_count++;
	rmutex_init(&d->lock);
	publish(d, slot, 0, 0, d->sectors);
	if (!d->rw(d, buf, 0, 1, 0) && buf[510] == 0x55 && buf[511] == 0xaa) {
		struct mbr_entry *e = (void *)(buf + 446);
		int gpt = 0;
		for (unsigned i = 0; i < 4; i++)
			if (e[i].type == 0xee)
				gpt = 1;
		if (gpt)
			scan_gpt(d, slot, buf);
		else
			scan_mbr(d, slot, buf);
	}
	int partitioned = 0;
	for (unsigned i = 1; i < FIXED_PARTS; i++)
		if (parts[slot][i].sectors)
			partitioned = 1;
	if (!partitioned)
		blockdev_update(d->name, d->sectors * d->sector_size,
				BLOCKDEV_FLAG_VOLUME);
	free(buf);
	return 0;
}
void *storage_map_bar(unsigned pci, unsigned bar, uint64_t minimum)
{
	pci_resource r[7];
	pci_get_resources(pci, r);
	uint64_t start = r[bar].start, end = start + r[bar].size;
	if ((r[bar].flags & 1) || r[bar].size < minimum ||
	    start < DEVICE_IO_BEGIN || end > DEVICE_IO_END || end <= start)
		return NULL;
	for (uint64_t p = start & PAGE_SIZE_MASK; p < end; p += PAGE_SIZE) {
		if (mm_map_io(p) != 1)
			return NULL;
		mm_set_map_flag(p, PAGE_ENTRY_KERNEL_DATA | PAGE_ENTRY_CD |
					   PAGE_ENTRY_WT);
		arch_mm_invalidate(p);
	}
	pci_write_field(pci, PCI_COMMAND, 2,
			pci_read_field(pci, PCI_COMMAND, 2) | 6 | 0x400);
	return (void *)(uintptr_t)start;
}
