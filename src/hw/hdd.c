#include <lock.h>
#include <int.h>
#include <block.h>
#include <time.h>
#include <config.h>
#include <klib.h>
#include <rbtree.h>
#include <list.h>
#include <port.h>
#include <macro.h>

/* The code in this file is an interface to an ATA (IDE)
   controller.  It attempts to comply to [ATA-3]. */

/* ATA command block port addresses. */
#define reg_data(CHANNEL) ((CHANNEL)->reg_base + 0) /* Data. */
#define reg_error(CHANNEL) ((CHANNEL)->reg_base + 1) /* Error. */
#define reg_nsect(CHANNEL) ((CHANNEL)->reg_base + 2) /* Sector Count. */
#define reg_lbal(CHANNEL) ((CHANNEL)->reg_base + 3) /* LBA 0:7. */
#define reg_lbam(CHANNEL) ((CHANNEL)->reg_base + 4) /* LBA 15:8. */
#define reg_lbah(CHANNEL) ((CHANNEL)->reg_base + 5) /* LBA 23:16. */
#define reg_device(CHANNEL) ((CHANNEL)->reg_base + 6) /* Device/LBA 27:24. */
#define reg_status(CHANNEL) ((CHANNEL)->reg_base + 7) /* Status (r/o). */
#define reg_command(CHANNEL) reg_status(CHANNEL) /* Command (w/o). */

/* ATA control block port addresses.
   (If we supported non-legacy ATA controllers this would not be
   flexible enough, but it's fine for what we do.) */
#define reg_ctl(CHANNEL) ((CHANNEL)->reg_base + 0x206) /* Control (w/o). */
#define reg_alt_status(CHANNEL) reg_ctl(CHANNEL) /* Alt Status (r/o). */

/* Alternate Status Register bits. */
#define STA_BSY 0x80 /* Busy. */
#define STA_DRDY 0x40 /* Device Ready. */
#define STA_DRQ 0x08 /* Data Request. */

/* Control Register bits. */
#define CTL_SRST 0x04 /* Software Reset. */

/* Device Register bits. */
#define DEV_MBS 0xa0 /* Must be set. */
#define DEV_LBA 0x40 /* Linear based addressing. */
#define DEV_DEV 0x10 /* Select device: 0=master, 1=slave. */

/* Commands.
   Many more are defined but this is the small subset that we
   use. */
#define CMD_IDENTIFY_DEVICE 0xec /* IDENTIFY DEVICE. */
#define CMD_READ_SECTOR_RETRY 0x20 /* READ SECTOR with retries. */
#define CMD_READ_SECTOR_RETRY_EXT 0x24 /* READ SECTOR with retries. */
#define CMD_WRITE_SECTOR_RETRY 0x30 /* WRITE SECTOR with retries. */
#define CMD_WRITE_SECTOR_RETRY_EXT 0x34 /* WRITE SECTOR with retries. */

/* An ATA device. */
typedef struct _ata_disk {
	char name[8]; /* Name, e.g. "hda". */
	struct _channel *channel; /* Channel that disk is attached to. */
	int dev_no; /* Device 0 or 1 for master or slave. */
	int is_ata; /* Is device an ATA disk? */
} ata_disk;

/* An ATA channel (aka controller).
   Each channel can control up to two disks. */
typedef struct _channel {
	char name[8]; /* Name, e.g. "ide0". */
	unsigned short reg_base; /* Base I/O port. */
	unsigned char irq; /* Interrupt in use. */
	spinlock_t iolock;
	cond_t event;
	ata_disk devices[2]; /* The devices on this channel. */
} channel;

#if HDD_CACHE_OPEN
/*
 * ---------------------------------------------------------------------------
 * Filesystem HDD Cache (sector cache)
 *
 * Design:
 * - Keyed by head sector (HEAD_SECTOR) for fixed-size group PREREAD_SECTOR.
 * - Hash table provides O(log n) lookup via rb-tree-based hash_find().
 * - Doubly-linked timer_list_head acts as an LRU: head = MRU, tail = LRU.
 * - Cache hit: move item to MRU (list tail in this file's conventions).
 * - Cache miss:
 *     - If under capacity: allocate a new cache item and read from disk.
 *     - If full: choose LRU (tail), flush it if dirty, reuse it for the
 *       requested head sector.
 * - Write policy: controlled by HDD_CACHE_WRITE_POLICY.
 *     - WRITE_BACK: mark dirty; flush later on eviction.
 *     - WRITE_THOUGH: flush immediately on updates.
 * - Accounting:
 *     - p->cache.sectors tracks total cached sectors.
 *     - fs_cache_size/max_fs_cache_size expose live/peak sizes.
 *     - cache_hit/cache_search_time/count track lookup performance.
 * ---------------------------------------------------------------------------
 */
#define CACHE_SECTOR_COUNT ((HDD_CACHE_SIZE * PAGE_SIZE) / BLOCK_SECTOR_SIZE)
#define SECTOR_PER_PAGE (PAGE_SIZE / BLOCK_SECTOR_SIZE)
#define PREREAD_SECTOR 1
#define HEAD_SECTOR(sector) (sector / PREREAD_SECTOR * PREREAD_SECTOR)
#define SECTOR_OFF(sector) (sector - HEAD_SECTOR(sector))

typedef struct _block_cache_item {
	/* Starting head sector for this cached extent (size PREREAD_SECTOR). */
	int sector;
	/* Dirty flag (WRITE_BACK policy): needs flush before reuse/eviction. */
	unsigned dirty;
	/* Virtual memory buffer holding cached sector(s). */
	void *buf;
	struct rb_node hash_node;
	list_entry time_list;
} block_cache_item;

typedef struct _block_cache {
	/* Red-black tree hash keyed by head sector for fast lookup. */
	hash_table *hash;
	/* LRU queue: tail is most recently used, head is least recently used. */
	list_entry timer_list_head;
	/* Number of cached sectors. */
	int sectors;
} block_cache;

static int int_comp(void *key1, void *key2)
{
	int k1 = (int)key1;
	int k2 = (int)key2;

	return (k1 - k2);
}

static block_cache_item *block_cache_item_create()
{
	unsigned buf_size = BLOCK_SECTOR_SIZE * PREREAD_SECTOR / PAGE_SIZE;
	if (!buf_size)
		buf_size = 1;
	/* Allocate metadata and a VM-backed buffer for cache payload. */
	block_cache_item *item = kmalloc(sizeof(*item));
	item->sector = -1;
	item->dirty = 0;
	item->buf = vm_alloc(buf_size); // kmalloc(BLOCK_SECTOR_SIZE);
	rb_init_node(&item->hash_node);
	list_init(&item->time_list);
	return item;
}

static void block_cache_item_remove(block_cache_item *item)
{
	unsigned buf_size = BLOCK_SECTOR_SIZE * PREREAD_SECTOR / PAGE_SIZE;
	if (!buf_size)
		buf_size = 1;

	if (!item)
		return;

	// kfree(item->buf);
	vm_free(item->buf, buf_size);
	kfree(item);
}

#endif

typedef struct _partition {
	block *block; /* Underlying block device. */
	unsigned int start; /* First sector within device. */
	unsigned char bootable;
#if HDD_CACHE_OPEN
	block_cache cache;
	int cache_inited;
	mutex_t cache_lock;
#endif
} partition;

static void interrupt_handler(intr_frame *);
static void reset_channel(channel *);
static int check_device_type(ata_disk *);
static void identify_ata_device(ata_disk *);
static int wait_while_busy(const ata_disk *);
static void select_device(const ata_disk *);
static void select_device_wait(const ata_disk *);
static void issue_pio_command(channel *, unsigned char command);
static void input_sector(channel *, void *);
static void output_sector(channel *c, const void *sector);
static char *descramble_ata_string(char *, int size);
static void channel_wait_polling(channel *c);
static int disk_read(void *aux, unsigned sector, void *buf, unsigned len);
static int disk_write(void *aux, unsigned sector, void *buf, unsigned len);
static void disk_close(void *aux);
static void parse_partition(block *block);
static void read_partition_table(block *block, unsigned int sector,
				 unsigned int primary_extended_sector,
				 int *part_nr);
static void found_partition(block *block, unsigned char part_type,
			    unsigned int start, unsigned int size, int part_nr,
			    unsigned char bootable);

static int partition_read(void *aux, unsigned sector, void *buf, unsigned len);
static int partition_write(void *aux, unsigned sector, void *buf, unsigned len);
static void partition_close(void *aux);
#if HDD_CACHE_OPEN
static int partition_cache_read(void *aux, unsigned sector, void *buf,
				unsigned len);
static int partition_cache_write(void *aux, unsigned sector, void *buf,
				 unsigned len);
#endif

/* We support the two "legacy" ATA channels found in a standard PC. */
#define CHANNEL_CNT 2
static channel channels[CHANNEL_CNT];

static void hdd_init()
{
	int chan_no;

	for (chan_no = 0; chan_no < CHANNEL_CNT; chan_no++) {
		channel *c = &channels[chan_no];
		int dev_no;

		c->name[0] = 'i';
		c->name[1] = 'd';
		c->name[2] = 'e';
		c->name[3] = '0' + chan_no;
		c->name[4] = '\0';
		/* Initialize channel. */
		switch (chan_no) {
		case 0:
			c->reg_base = 0x1f0;
			c->irq = 14 + 0x20;
			break;
		case 1:
			c->reg_base = 0x170;
			c->irq = 15 + 0x20;
			break;
		default:
			printk("fatal error\n");
		}
#ifdef DEBUG
		printk("channel %d, name %s\n", chan_no, c->name);
#endif
		cond_init(&c->event, c->name, 1);
		spinlock_init_ex(&c->iolock, 0);
		/* Initialize devices. */
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

		/* Register interrupt handler. */
		int_register(c->irq, interrupt_handler, 0, 0);

		/* Reset hardware. */
		reset_channel(c);

		/* Distinguish ATA hard disks from other devices. */
		if (check_device_type(&c->devices[0]))
			check_device_type(&c->devices[1]);

#ifdef DEBUG
		printk("channel %d, dev %d, name %s, is_ata %d\n", chan_no, 0,
		       c->devices[0].name, c->devices[0].is_ata);
		printk("channel %d, dev %d, name %s, is_ata %d\n", chan_no, 1,
		       c->devices[1].name, c->devices[1].is_ata);
#endif

		/* Read hard disk identity information. */
		for (dev_no = 0; dev_no < 2; dev_no++)
			if (c->devices[dev_no].is_ata)
				identify_ata_device(&c->devices[dev_no]);
	}
}

static void interrupt_handler(intr_frame *f)
{
	channel *c;

	for (c = channels; c < channels + CHANNEL_CNT; c++)
		if (f->vec_no == c->irq) {
			port_read_byte(
				reg_status(c)); /* Acknowledge interrupt. */

			cond_notify_at_intr(&c->event); /* Wake up waiter. */

			return;
		}
}

static void reset_channel(channel *c)
{
	int present[2];
	int dev_no;

	/* The ATA reset sequence depends on which devices are present,
       so we start by detecting device presence. */
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

	/* Issue soft reset sequence, which selects device 0 as a side effect.
       Also enable interrupts. */
	port_write_byte(reg_ctl(c), 0);
	usleep(10);
	port_write_byte(reg_ctl(c), CTL_SRST);
	usleep(10);
	port_write_byte(reg_ctl(c), 0);

	delay(150000);

	/* Wait for device 0 to clear BSY. */
	if (present[0]) {
		select_device(&c->devices[0]);
		wait_while_busy(&c->devices[0]);
	}

	/* Wait for device 1 to clear BSY. */
	if (present[1]) {
		int i;

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

static int wait_while_busy(const ata_disk *d)
{
	channel *c = d->channel;
	int i;
	int ret;

	for (i = 0; i < 3000; i++) {
		if (i == 700)
			printk("%s: busy, waiting...", d->name);
		if (!(port_read_byte(reg_alt_status(c)) & STA_BSY)) {
			if (i >= 700)
				printk("ok\n");
			ret = (port_read_byte(reg_alt_status(c)) & STA_DRQ);
			if (ret == 0) {
			}
			return 1;
		}
		delay(10000);
	}

	printf("failed\n");
	return 0;
}

static void select_device(const ata_disk *d)
{
	channel *c = d->channel;
	unsigned char dev = DEV_MBS;
	if (d->dev_no == 1)
		dev |= DEV_DEV;
	port_write_byte(reg_device(c), dev);
	port_read_byte(reg_alt_status(c));
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
	    (status & STA_DRDY) == 0 || (status & STA_BSY) != 0) {
		d->is_ata = 0;
		return error != 0x81;
	} else {
		d->is_ata = (lbam == 0 && lbah == 0) ||
			    (lbam == 0x3c && lbah == 0xc3);
		return 1;
	}
}

static void identify_ata_device(ata_disk *d)
{
	channel *c = d->channel;
	char id[BLOCK_SECTOR_SIZE];
	unsigned int capacity;
	char *model, *serial;
	char extra_info[128];
	block *block;

	/* Send the IDENTIFY DEVICE command, wait for an interrupt
       indicating the device's response is ready, and read the data
       into our buffer. */
	select_device_wait(d);
	issue_pio_command(c, CMD_IDENTIFY_DEVICE);

	cond_wait_at_intr(&c->event);

	if (!wait_while_busy(d)) {
		d->is_ata = 0;
		return;
	}

	input_sector(c, id);

	/* Calculate capacity.
       Read model name and serial number. */
	capacity = *(unsigned int *)&id[60 * 2];
	model = descramble_ata_string(&id[10 * 2], 20);
	serial = descramble_ata_string(&id[27 * 2], 40);
	strcpy(extra_info, "model \"");
	strcat(extra_info, model);
	strcat(extra_info, "\", serial \"");
	strcat(extra_info, serial);
	strcat(extra_info, "\"");
#ifdef DEBUG
	printk("%s: capacity %h, model %s, serial %s\n", d->name,
	       capacity * BLOCK_SECTOR_SIZE, model, serial);
	printk("%s: extra_info %s\n", d->name, extra_info);
#endif

	/* Disable access to IDE disks over 1 GB, which are likely
       physical IDE disks rather than virtual ones.  If we don't
       allow access to those, we're less likely to scribble on
       someone's important data.  You can disable this check by
       hand if you really want to do so. */
	if (capacity >= 1024 * 1024 * 1024 / BLOCK_SECTOR_SIZE) {
		printk("%s: ignoring %h disk for safety\n", d->name,
		       capacity * BLOCK_SECTOR_SIZE);
		d->is_ata = 0;
		return;
	}

	block = block_register(d, d->name, disk_read, disk_write, disk_close,
			       BLOCK_RAW, capacity);
	parse_partition(block);
}

static void parse_partition(block *block)
{
	int count = 0;

	read_partition_table(block, 0, 0, &count);
}

static void read_partition_table(block *block, unsigned int sector,
				 unsigned int primary_extended_sector,
				 int *part_nr)
{
	/* Format of a partition table entry.  See [Partitions]. */
	struct partition_table_entry {
		unsigned char bootable; /* 0x00=not bootable, 0x80=bootable. */
		unsigned char start_chs
			[3]; /* Encoded starting cylinder, head, sector. */
		unsigned char
			type; /* Partition type (see partition_type_name). */
		unsigned char
			end_chs[3]; /* Encoded ending cylinder, head, sector. */
		unsigned int
			offset; /* Start sector offset from partition table. */
		unsigned int size; /* Number of sectors. */
	} __attribute__((packed));

	/* Partition table sector. */
	struct partition_table {
		unsigned char
			loader[446]; /* Loader, in top-level partition table. */
		struct partition_table_entry partitions[4]; /* Table entries. */
		unsigned short signature; /* Should be 0xaa55. */
	} __attribute__((packed));

	struct partition_table *pt;
	unsigned i;
	int found = 0;

	/* Check SECTOR validity. */
	if (sector >= block->sector_size) {
		printk("%s: Partition table at sector %d past end of device.\n",
		       block->name, sector);
		return;
	}

	/* Read sector. */
	pt = (struct partition_table *)kmalloc(sizeof *pt);
	if (pt == 0) {
		printk("Failed to allocate memory for partition table.");
		return;
	}
	block->read(block->aux, sector, pt, BLOCK_SECTOR_SIZE);

	/* Parse partitions. */
	for (i = 0; i < sizeof pt->partitions / sizeof *pt->partitions; i++) {
		struct partition_table_entry *e = &pt->partitions[i];

		if (e->size == 0 || e->type == 0) {
#ifdef DEBUG
			printk("empty partition\n");
#endif
			/* Ignore empty partition. */
		} else if (e->type == 0x05 /* Extended partition. */
			   ||
			   e->type == 0x0f /* Windows 98 extended partition. */
			   || e->type == 0x85 /* Linux extended partition. */
			   || e->type == 0xc5) /* DR-DOS extended partition. */
		{
			printk("%s: Extended partition in sector %d\n",
			       block->name, sector);

			/* The interpretation of the offset field for extended
               partitions is bizarre.  When the extended partition
               table entry is in the master boot record, that is,
               the device's primary partition table in sector 0, then
               the offset is an absolute sector number.  Otherwise,
               no matter how deep the partition table we're reading
               is nested, the offset is relative to the start of
               the extended partition that the MBR points to. */
			if (sector == 0)
				read_partition_table(block, e->offset,
						     e->offset, part_nr);
			else
				read_partition_table(
					block,
					e->offset + primary_extended_sector,
					primary_extended_sector, part_nr);
		} else {
			++*part_nr;

			found_partition(block, e->type, e->offset + sector,
					e->size, *part_nr, e->bootable);
		}
	}

	if (!*part_nr) {
#ifdef DEBUG
		printk("use default\n");
#endif
		// no partition table info, assume this disk only has one raw partition
		found_partition(block, 0x21, sector, block->sector_size, 0, 1);
	}
	kfree(pt);
}

#if HDD_CACHE_OPEN
unsigned cache_hit = 0;
unsigned long long cache_search_time = 0;
unsigned cache_search_count = 0;
unsigned fs_cache_size = 0;
unsigned max_fs_cache_size = 0;

static void init_partition_cache(partition *p)
{
	int i = 0;

	/* Initialize hash + LRU list and per-partition lock. */
	p->cache.sectors = 0;
	p->cache.hash = hash_create(int_comp);
	list_init(&p->cache.timer_list_head);
	p->cache_inited = 1;
	mutex_init(&p->cache_lock);
}

static block_cache_item *hdd_cache_lookup(partition *p, int sector)
{
	int i = 0;
	unsigned long long search_time = 0;
	int head_sector = HEAD_SECTOR(sector);
	key_value_pair *pair = 0;
	block_cache_item *ret = 0;
	if (sector == -1) {
		return -1;
	}

	cache_search_count++;
	if (TestControl.profiling)
		search_time = time_now_us();

	/* Hash lookup by head sector. */
	pair = hash_find(p->cache.hash, head_sector);

	if (TestControl.profiling) {
		search_time = time_now_us() - search_time;
		cache_search_time += search_time;
	}

	if (!pair)
		return 0;

	ret = pair->val;
	cache_hit++;

	return ret;
}

static block_cache_item *hdd_cache_find_empty(partition *p, int sector)
{
	block_cache_item *item;
	int head_sector = HEAD_SECTOR(sector);
	if (p->cache.sectors >= CACHE_SECTOR_COUNT) {
		return 0;
	}

	/* Create new cache item and link into LRU + hash. */
	item = block_cache_item_create();
	item->sector = head_sector;
	list_insert_tail(&p->cache.timer_list_head, &item->time_list);
	hash_insert(p->cache.hash, head_sector, item);
	p->cache.sectors += PREREAD_SECTOR;
	fs_cache_size = p->cache.sectors;
	if (max_fs_cache_size < p->cache.sectors)
		max_fs_cache_size = p->cache.sectors;
	return item;
}

static block_cache_item *hdd_cache_find_oldest(partition *p)
{
	list_entry *node;
	block_cache_item *item = 0;

	if (p->cache.sectors == 0)
		return 0;

	/* LRU policy: pick the least recently used at list head. */
	node = p->cache.timer_list_head.prev;
	item = container_of(node, block_cache_item, time_list);
	return item;
}

static void hdd_cache_flush(partition *p, block_cache_item *item)
{
	char *buf = item->buf;
	int sector = item->sector;
	int head_sector = sector;
	int i = 0;

	if (sector < 0)
		return;

	/* Flush dirty sectors to the underlying device. */
	if (item->dirty) {
		for (i = 0; i < PREREAD_SECTOR; i++)
			partition_write(p, head_sector + i,
					buf + i * BLOCK_SECTOR_SIZE,
					BLOCK_SECTOR_SIZE);

		item->dirty = 0;
	}
}

static void hdd_cache_update(partition *p, block_cache_item *item, int sector,
			     void *buf, int mark_dirty)
{
	/* Update cached payload; remap if the head sector changed. */
	int head_sector = HEAD_SECTOR(sector);
	int same_sector = (item->sector == head_sector);
	int sector_off = SECTOR_OFF(sector);
	if (!same_sector) {
		hash_remove(p->cache.hash, item->sector);
		hash_insert(p->cache.hash, head_sector, item);
	}
	item->dirty |= mark_dirty;
	item->sector = head_sector;

	/* Move to MRU position. */
	list_remove_entry(&item->time_list);
	list_insert_tail(&p->cache.timer_list_head, &item->time_list);

	memcpy((char *)item->buf + sector_off * BLOCK_SECTOR_SIZE, buf,
	       BLOCK_SECTOR_SIZE);
#if HDD_CACHE_WRITE_POLICY == HDD_CACHE_WRITE_THOUGH
	if (mark_dirty) {
		hdd_cache_flush(p, item);
	}
#endif
}

static void hdd_cache_update_all(void *aux, partition *p,
				 block_cache_item *volatile item,
				 int head_sector, int mark_dirty)
{
	/* Replace cached extent and pull in PREREAD_SECTOR sectors from disk. */
	int i = 0;
	int same_sector = (item->sector == head_sector);
	if (!same_sector) {
		hash_remove(p->cache.hash, item->sector);
		hash_insert(p->cache.hash, head_sector, item);
	}

	item->dirty |= mark_dirty;
	item->sector = head_sector;

	/* Move to MRU position before filling data. */
	list_remove_entry(&item->time_list);
	list_insert_tail(&p->cache.timer_list_head, &item->time_list);
	for (i = 0; i < PREREAD_SECTOR; i++)
		partition_read(aux, head_sector + i,
			       (char *)item->buf + i * BLOCK_SECTOR_SIZE,
			       BLOCK_SECTOR_SIZE);
#if HDD_CACHE_WRITE_POLICY == HDD_CACHE_WRITE_THOUGH
	if (mark_dirty) {
		hdd_cache_flush(p, item);
	}
#endif
}

static void flush_partition_cache(partition *p)
{
	/* Drain all cached items in LRU order, flushing dirty ones. */
	list_entry *entry = p->cache.timer_list_head.prev;
	list_entry *next;
	while (entry != &p->cache.timer_list_head) {
		next = entry->prev;
		block_cache_item *item =
			container_of(entry, block_cache_item, time_list);
		if (item && item->sector != -1 && item->dirty) {
			hdd_cache_flush(p, item);
		}
		block_cache_item_remove(item);
		p->cache.sectors -= PREREAD_SECTOR;
		fs_cache_size = p->cache.sectors;
		entry = next;
	}
}

unsigned fs_cache_read_size = 0;
unsigned fs_cache_write_size = 0;

static int partition_cache_read(void *aux, unsigned sector, void *buf,
				unsigned len)
{
	int i = 0;
	partition *p = aux;
	int index = 0;
	block_cache_item *volatile item = 0;
	int sector_off = SECTOR_OFF(sector);
	int head_sector = HEAD_SECTOR(sector);

	mutex_lock(&p->cache_lock);
	/* Find cache */
	item = hdd_cache_lookup(p, sector);
	if (item) {
		/* Promote to MRU and satisfy from cache. */
		list_remove_entry(&item->time_list);
		list_insert_tail(&p->cache.timer_list_head, &item->time_list);
		mutex_unlock(&p->cache_lock);
		memcpy(buf, (char *)item->buf + sector_off * BLOCK_SECTOR_SIZE,
		       BLOCK_SECTOR_SIZE);
		fs_cache_read_size += BLOCK_SECTOR_SIZE;
		return BLOCK_SECTOR_SIZE;
	}

	/* Cache not found, try to find an empty slot. */
	item = hdd_cache_find_empty(p, sector);
	if (!item) {
		/* Cache not found, and no more empty slot, flush oldest one */
		item = hdd_cache_find_oldest(p);
		if (item) {
			hdd_cache_flush(p, item);
		}
	}

	/* Empty slot or oldest one. */
	if (item) {
		/* Read through to cache, then return the requested sector. */
		hdd_cache_update_all(aux, p, item, head_sector, 0);
		mutex_unlock(&p->cache_lock);
		memcpy(buf, (char *)item->buf + sector_off * BLOCK_SECTOR_SIZE,
		       BLOCK_SECTOR_SIZE);
		fs_cache_read_size += BLOCK_SECTOR_SIZE;
		return BLOCK_SECTOR_SIZE;
	}

	mutex_unlock(&p->cache_lock);
	return -1;
}

static int partition_cache_write(void *aux, unsigned sector, void *buf,
				 unsigned len)
{
	partition *p = aux;
	block_cache_item *item = 0;

	mutex_lock(&p->cache_lock);
	/* Find cache */
	item = hdd_cache_lookup(p, sector);
	if (item) {
		/* Update payload and mark dirty; policy controls flush timing. */
		hdd_cache_update(p, item, sector, buf, 1);
		mutex_unlock(&p->cache_lock);
		fs_cache_write_size += BLOCK_SECTOR_SIZE;
		return BLOCK_SECTOR_SIZE;
	}

	/* Cache not found, try to find an empty slot. */
	item = hdd_cache_find_empty(p, sector);
	if (item) {
		hdd_cache_update(p, item, sector, buf, 1);
		mutex_unlock(&p->cache_lock);
		fs_cache_write_size += len;
		return len;
	}

	/* Cache not found, and no more empty slot, flush oldest one */
	item = hdd_cache_find_oldest(p);
	if (item) {
		/* Evict LRU (with flush if dirty) and reuse the entry. */
		hdd_cache_flush(p, item);
		hdd_cache_update(p, item, sector, buf, 1);
		mutex_unlock(&p->cache_lock);
		fs_cache_write_size += len;
		return len;
	}

	mutex_unlock(&p->cache_lock);
	return -1;
}

#endif

static void found_partition(block *block, unsigned char part_type,
			    unsigned int start, unsigned int size, int part_nr,
			    unsigned char bootable)
{
	if (start >= block->sector_size)
		printk("%s%d: Partition starts past end of device (sector %d)\n",
		       block->name, part_nr, start);
	else if (start + size < start || start + size > block->sector_size)
		printk("%s%d: Partition end (%d) past end of device (%d)\n",
		       block->name, part_nr, start + size, block->sector_size);
	else {
		block_type type = (part_type == 0x20 ? BLOCK_KERNEL :
				   part_type == 0x21 ? BLOCK_FILESYS :
				   part_type == 0x83 ? BLOCK_LINUX :
				   part_type == 0x22 ? BLOCK_SCRATCH :
				   part_type == 0x23 ? BLOCK_SWAP :
						       BLOCK_UNKNOW);
		partition *p;
		struct _block *b;
		char extra_info[128];
		char name[16];
		char ext[2];
		unsigned z = sizeof(*p) / PAGE_SIZE + 1;
		p = (partition *)vm_alloc(z);
		if (p == 0) {
			printk("Failed to allocate memory for partition descriptor");
			return;
		}
		p->block = block;
		p->start = start;
		p->bootable = bootable;
		strcpy(name, block->name);
		ext[0] = '0' + part_nr;
		ext[1] = '\0';
		strcat(name, ext);

#if HDD_CACHE_OPEN
		p->cache_inited = 0;

		init_partition_cache(p);
#endif

#ifdef DEBUG
		printk("got a partition, with start %d, bootable %x\n",
		       p->start, p->bootable);
#endif

#if HDD_CACHE_OPEN
		b = block_register(p, name, partition_cache_read,
				   partition_cache_write, partition_close, type,
				   size);
#else
		b = block_register(p, name, partition_read, partition_write,
				   partition_close, type, size);
#endif

		if (type == BLOCK_LINUX) {
			ext4_blockdev_register(b, name, BLOCK_SECTOR_SIZE,
					       size);
		}
	}
}

static void wait_until_idle(const ata_disk *d)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if ((port_read_byte(reg_status(d->channel)) &
		     (STA_BSY | STA_DRQ)) == 0)
			return;
		delay(10);
	}

	printk("%s: idle timeout\n", d->name);
}

/* Wait for the device to be selected and mark that we expect an interrupt. */
static void select_device_wait(const ata_disk *d)
{
	channel *c = d->channel;
	cond_reset(&c->event);
	select_device(d);
}

/* Issue a PIO command to the selected channel. */
static void issue_pio_command(channel *c, unsigned char command)
{
	port_write_byte(reg_command(c), command);
}

/* Read a single 512-byte sector via PIO into a buffer. */
static void input_sector(channel *c, void *sector)
{
	port_read_dwords(reg_data(c), sector, BLOCK_SECTOR_SIZE / 4);
}

/* Write a single 512-byte sector via PIO from a buffer. */
static void output_sector(channel *c, const void *sector)
{
	port_write_dwords(reg_data(c), sector, BLOCK_SECTOR_SIZE / 4);
}

/* Decode ATA strings by swapping byte pairs and trimming trailing blanks. */
static char *descramble_ata_string(char *string, int size)
{
	int i;

	/* Swap all pairs of bytes. */
	for (i = 0; i + 1 < size; i += 2) {
		char tmp = string[i];
		string[i] = string[i + 1];
		string[i + 1] = tmp;
	}

	/* Find the last non-white, non-null character. */
	for (size--; size > 0; size--) {
		int c = string[size - 1];
		if (c != '\0' && !isspace(c))
			break;
	}
	string[size] = '\0';

	return string;
}

/* Program LBA registers for a one-sector PIO transfer. */
static void select_sector(ata_disk *d, unsigned int sec_no)
{
	channel *c = d->channel;

	select_device_wait(d);
	port_write_byte(reg_nsect(c), 1);
	port_write_byte(reg_lbal(c), sec_no);
	port_write_byte(reg_lbam(c), sec_no >> 8);
	port_write_byte(reg_lbah(c), (sec_no >> 16));
	port_write_byte(reg_device(c), DEV_MBS | DEV_LBA |
					       (d->dev_no == 1 ? DEV_DEV : 0) |
					       (sec_no >> 24));
}

unsigned disk_read_size = 0;
unsigned disk_write_size = 0;

/* Synchronous single-sector read. Returns bytes read (len on success). */
static int disk_read(void *aux, unsigned sec_no, void *buf, unsigned len)
{
	ata_disk *volatile d = aux;
	channel *volatile c = d->channel;
	spinlock_lock(&c->iolock);

	select_sector(d, sec_no);

	issue_pio_command(c, CMD_READ_SECTOR_RETRY);

	cond_wait_at_intr(&c->event);

	input_sector(c, buf);

	spinlock_unlock(&c->iolock);

	disk_read_size += len;

	return len;
}

/* Synchronous single-sector write. Returns bytes written (len on success). */
static int disk_write(void *aux, unsigned sec_no, void *buf, unsigned len)
{
	ata_disk *d = aux;
	channel *c = d->channel;
	spinlock_lock(&c->iolock);

	select_sector(d, sec_no);

	issue_pio_command(c, CMD_WRITE_SECTOR_RETRY);

	output_sector(c, buf);

	cond_wait_at_intr(&c->event);

	spinlock_unlock(&c->iolock);

	disk_write_size += len;

	return len;
}

/* Placeholder for disk close; no resources to release here. */
static void disk_close(void *aux)
{
}

static int partition_read(void *aux, unsigned sector, void *buf, unsigned len)
{
	partition *p = aux;
	return p->block->read(p->block->aux, p->start + sector, buf, len);
}

static int partition_write(void *aux, unsigned sector, void *buf, unsigned len)
{
	partition *p = aux;
	return p->block->write(p->block->aux, p->start + sector, buf, len);
}

static void partition_close(void *aux)
{
#if HDD_CACHE_OPEN
	int i = 0;
	partition *p = aux;
	if (p->cache_inited) {
		flush_partition_cache(aux);
		/* Destroy hash to avoid memory leaks; reset LRU head. */
		hash_destroy(p->cache.hash);
		p->cache.hash = 0;
		list_init(&p->cache.timer_list_head);
		p->cache_inited = 0;
	}
#endif
}

KERNEL_INIT(2, hdd_init);
