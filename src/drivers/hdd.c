#include <hdd.h>
#include <lock.h>
#include <int.h>
#include <block.h>
#include <timer.h>
#include <config.h>
#include <klib.h>
#include <rbtree.h>
#include <list.h>

/* The code in this file is an interface to an ATA (IDE)
   controller.  It attempts to comply to [ATA-3]. */

   /* ATA command block port addresses. */
#define reg_data(CHANNEL) ((CHANNEL)->reg_base + 0)     /* Data. */
#define reg_error(CHANNEL) ((CHANNEL)->reg_base + 1)    /* Error. */
#define reg_nsect(CHANNEL) ((CHANNEL)->reg_base + 2)    /* Sector Count. */
#define reg_lbal(CHANNEL) ((CHANNEL)->reg_base + 3)     /* LBA 0:7. */
#define reg_lbam(CHANNEL) ((CHANNEL)->reg_base + 4)     /* LBA 15:8. */
#define reg_lbah(CHANNEL) ((CHANNEL)->reg_base + 5)     /* LBA 23:16. */
#define reg_device(CHANNEL) ((CHANNEL)->reg_base + 6)   /* Device/LBA 27:24. */
#define reg_status(CHANNEL) ((CHANNEL)->reg_base + 7)   /* Status (r/o). */
#define reg_command(CHANNEL) reg_status (CHANNEL)       /* Command (w/o). */

/* ATA control block port addresses.
   (If we supported non-legacy ATA controllers this would not be
   flexible enough, but it's fine for what we do.) */
#define reg_ctl(CHANNEL) ((CHANNEL)->reg_base + 0x206)  /* Control (w/o). */
#define reg_alt_status(CHANNEL) reg_ctl (CHANNEL)       /* Alt Status (r/o). */

   /* Alternate Status Register bits. */
#define STA_BSY 0x80            /* Busy. */
#define STA_DRDY 0x40           /* Device Ready. */
#define STA_DRQ 0x08            /* Data Request. */

/* Control Register bits. */
#define CTL_SRST 0x04           /* Software Reset. */

/* Device Register bits. */
#define DEV_MBS 0xa0            /* Must be set. */
#define DEV_LBA 0x40            /* Linear based addressing. */
#define DEV_DEV 0x10            /* Select device: 0=master, 1=slave. */

/* Commands.
   Many more are defined but this is the small subset that we
   use. */
#define CMD_IDENTIFY_DEVICE 0xec        /* IDENTIFY DEVICE. */
#define CMD_READ_SECTOR_RETRY 0x20      /* READ SECTOR with retries. */
#define CMD_READ_SECTOR_RETRY_EXT 0x24      /* READ SECTOR with retries. */
#define CMD_WRITE_SECTOR_RETRY 0x30     /* WRITE SECTOR with retries. */
#define CMD_WRITE_SECTOR_RETRY_EXT 0x34     /* WRITE SECTOR with retries. */

   /* An ATA device. */
typedef struct _ata_disk
{
    char name[8];               /* Name, e.g. "hda". */
    struct _channel *channel;    /* Channel that disk is attached to. */
    int dev_no;                 /* Device 0 or 1 for master or slave. */
    int is_ata;                /* Is device an ATA disk? */
}ata_disk;

/* An ATA channel (aka controller).
   Each channel can control up to two disks. */
typedef struct _channel
{
    char name[8];               /* Name, e.g. "ide0". */
    unsigned short reg_base;          /* Base I/O port. */
    unsigned char irq;                /* Interrupt in use. */
    int expecting_interrupt;   /* True if an interrupt is expected, false if
                                   any interrupt would be spurious. */
    semaphore iolock;
    semaphore sema;
    ata_disk devices[2];     /* The devices on this channel. */
}channel;

#if HDD_CACHE_OPEN

#define CACHE_SECTOR_COUNT ((HDD_CACHE_SIZE * PAGE_SIZE) / BLOCK_SECTOR_SIZE)
#define SECTOR_PER_PAGE (PAGE_SIZE / BLOCK_SECTOR_SIZE)

typedef struct _block_cache_item
{
    int sector;
    unsigned last_time;
    unsigned dirty;
    void* buf;
    struct rb_node hash_node;
    LIST_ENTRY time_list;
}block_cache_item;

typedef struct _block_cache
{
    hash_table* hash;
    LIST_ENTRY timer_list_head;
    int count;
}block_cache;

static int int_comp(void* key1, void* key2)
{
    int k1 = (int)key1;
    int k2 = (int)key2;

    return (k1 - k2);
}

static block_cache_item* block_cache_item_create()
{
    block_cache_item* item = kmalloc(sizeof(*item));
    item->sector = -1;
    item->last_time = 0;
    item->dirty = 0;
    item->buf = kmalloc(BLOCK_SECTOR_SIZE);
    rb_init_node(&item->hash_node);
    InitializeListHead(&item->time_list);
    return item;
}

static void block_cache_item_remove(block_cache_item* item)
{
    if (!item)
        return;

    kfree(item->buf);
    kfree(item);
}

#endif

typedef struct _partition
{
    block *block;                /* Underlying block device. */
    unsigned int start;               /* First sector within device. */
    unsigned char bootable;
#if HDD_CACHE_OPEN
    block_cache cache;
    int cache_inited;
#endif
}partition;





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
static int hdd_read(void* aux, unsigned sector, void* buf, unsigned len);
static int hdd_write(void* aux, unsigned sector, void* buf, unsigned len);
static void hdd_close(void* aux);
static void parse_partition(block *block);
static void read_partition_table(block *block, unsigned int sector,
    unsigned int primary_extended_sector,
    int *part_nr);
static void found_partition(block *block, unsigned char part_type,
    unsigned int start, unsigned int size,
    int part_nr, unsigned char bootable);

static int partition_read(void* aux, unsigned sector, void* buf, unsigned len);
static int partition_write(void* aux, unsigned sector, void* buf, unsigned len);
static void partition_close(void* aux);
#if HDD_CACHE_OPEN
static int partition_cache_read(void* aux, unsigned sector, void* buf, unsigned len);
static int partition_cache_write(void* aux, unsigned sector, void* buf, unsigned len);
#endif

/* We support the two "legacy" ATA channels found in a standard PC. */
#define CHANNEL_CNT 2
static channel channels[CHANNEL_CNT];

void hdd_init()
{
    int chan_no;

    for (chan_no = 0; chan_no < CHANNEL_CNT; chan_no++)
    {
        channel *c = &channels[chan_no];
        int dev_no;

        c->name[0] = 'i';
        c->name[1] = 'd';
        c->name[2] = 'e';
        c->name[3] = '0' + chan_no;
        c->name[4] = '\0';
        /* Initialize channel. */
        switch (chan_no)
        {
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
        c->expecting_interrupt = 0;
        sema_init(&c->sema, c->name, 1);
        sema_init(&c->iolock, "iolock", 0);
        /* Initialize devices. */
        for (dev_no = 0; dev_no < 2; dev_no++)
        {
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
        printk("channel %d, dev %d, name %s, is_ata %d\n", chan_no, 0, c->devices[0].name, c->devices[0].is_ata);
        printk("channel %d, dev %d, name %s, is_ata %d\n", chan_no, 1, c->devices[1].name, c->devices[1].is_ata);
#endif

        /* Read hard disk identity information. */
        for (dev_no = 0; dev_no < 2; dev_no++)
            if (c->devices[dev_no].is_ata)
                identify_ata_device(&c->devices[dev_no]);
    }

}

static void interrupt_handler(intr_frame * f)
{
    channel *c;

    for (c = channels; c < channels + CHANNEL_CNT; c++)
        if (f->vec_no == c->irq)
        {
            if (c->expecting_interrupt)
            {
                _read_port(reg_status(c));               /* Acknowledge interrupt. */
                //printk("[hdd] sema set \n");
                sema_trigger_at_intr(&c->sema);      /* Wake up waiter. */

                c->expecting_interrupt = 0;
            }
            else
                printk("%s: unexpected interrupt\n", c->name);
            return;
        }
}

static void reset_channel(channel * c)
{
    int present[2];
    int dev_no;

    /* The ATA reset sequence depends on which devices are present,
       so we start by detecting device presence. */
    for (dev_no = 0; dev_no < 2; dev_no++)
    {
        ata_disk *d = &c->devices[dev_no];

        select_device(d);

        _write_port(reg_nsect(c), 0x55);
        _write_port(reg_lbal(c), 0xaa);

        _write_port(reg_nsect(c), 0xaa);
        _write_port(reg_lbal(c), 0x55);

        _write_port(reg_nsect(c), 0x55);
        _write_port(reg_lbal(c), 0xaa);

        present[dev_no] = (_read_port(reg_nsect(c)) == 0x55
            && _read_port(reg_lbal(c)) == 0xaa);
    }

    /* Issue soft reset sequence, which selects device 0 as a side effect.
       Also enable interrupts. */
    _write_port(reg_ctl(c), 0);
    usleep(10);
    _write_port(reg_ctl(c), CTL_SRST);
    usleep(10);
    _write_port(reg_ctl(c), 0);

    delay(150000);

    /* Wait for device 0 to clear BSY. */
    if (present[0])
    {
        select_device(&c->devices[0]);
        wait_while_busy(&c->devices[0]);
    }

    /* Wait for device 1 to clear BSY. */
    if (present[1])
    {
        int i;

        select_device(&c->devices[1]);
        for (i = 0; i < 3000; i++)
        {
            if (_read_port(reg_nsect(c)) == 1 && _read_port(reg_lbal(c)) == 1)
                break;
            delay(10000);
        }
        wait_while_busy(&c->devices[1]);
    }
}

static int wait_while_busy(const ata_disk * d)
{
    channel *c = d->channel;
    int i;
    int ret;

    for (i = 0; i < 3000; i++)
    {
        if (i == 700)
            printk("%s: busy, waiting...", d->name);
        if (!(_read_port(reg_alt_status(c)) & STA_BSY))
        {
            if (i >= 700)
                printk("ok\n");
            ret = (_read_port(reg_alt_status(c)) & STA_DRQ);
            if (ret == 0)
            {
            }
            return 1;
        }
        delay(10000);
    }

    printf("failed\n");
    return 0;
}

static void select_device(const ata_disk * d)
{
    channel *c = d->channel;
    unsigned char dev = DEV_MBS;
    if (d->dev_no == 1)
        dev |= DEV_DEV;
    _write_port(reg_device(c), dev);
    _read_port(reg_alt_status(c));
}

static int check_device_type(ata_disk * d)
{
    channel *c = d->channel;
    unsigned char error, lbam, lbah, status;

    select_device(d);

    error = _read_port(reg_error(c));
    lbam = _read_port(reg_lbam(c));
    lbah = _read_port(reg_lbah(c));
    status = _read_port(reg_status(c));

    if ((error != 1 && (error != 0x81 || d->dev_no == 1))
        || (status & STA_DRDY) == 0
        || (status & STA_BSY) != 0)
    {
        d->is_ata = 0;
        return error != 0x81;
    }
    else
    {
        d->is_ata = (lbam == 0 && lbah == 0) || (lbam == 0x3c && lbah == 0xc3);
        return 1;
    }
}

static void identify_ata_device(ata_disk * d)
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

    //printk("[hdd] sema iden wait << \n");
    sema_wait_for_intr(&c->sema);
    //printk("[hdd] sema iden wait >> \n");

    if (!wait_while_busy(d))
    {
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
    printk("%s: capacity %h, model %s, serial %s\n", d->name, capacity*BLOCK_SECTOR_SIZE, model, serial);
    printk("%s: extra_info %s\n", d->name, extra_info);
#endif

    /* Disable access to IDE disks over 1 GB, which are likely
       physical IDE disks rather than virtual ones.  If we don't
       allow access to those, we're less likely to scribble on
       someone's important data.  You can disable this check by
       hand if you really want to do so. */
    if (capacity >= 1024 * 1024 * 1024 / BLOCK_SECTOR_SIZE)
    {
        printk("%s: ignoring %h disk for safety\n", d->name, capacity*BLOCK_SECTOR_SIZE);
        d->is_ata = 0;
        return;
    }

    block = block_register(d, d->name, hdd_read, hdd_write, hdd_close, BLOCK_RAW, capacity);
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
    struct partition_table_entry
    {
        unsigned char bootable;         /* 0x00=not bootable, 0x80=bootable. */
        unsigned char start_chs[3];     /* Encoded starting cylinder, head, sector. */
        unsigned char type;             /* Partition type (see partition_type_name). */
        unsigned char end_chs[3];       /* Encoded ending cylinder, head, sector. */
        unsigned int offset;          /* Start sector offset from partition table. */
        unsigned int size;            /* Number of sectors. */
    }
    __attribute__((packed));

    /* Partition table sector. */
    struct partition_table
    {
        unsigned char loader[446];      /* Loader, in top-level partition table. */
        struct partition_table_entry partitions[4];       /* Table entries. */
        unsigned short signature;       /* Should be 0xaa55. */
    }
    __attribute__((packed));

    struct partition_table *pt;
    unsigned i;
    int found = 0;

    /* Check SECTOR validity. */
    if (sector >= block->sector_size)
    {
        printk("%s: Partition table at sector %d past end of device.\n",
            block->name, sector);
        return;
    }

    /* Read sector. */
    pt = (struct partition_table *)kmalloc(sizeof *pt);
    if (pt == 0)
    {
        printk("Failed to allocate memory for partition table.");
        return;
    }
    block->read(block->aux, 0, pt, BLOCK_SECTOR_SIZE);

    /* Check signature. */
    //if (pt->signature != 0xaa55)
    if (0)
    {
        if (primary_extended_sector == 0)
            printk("%s: Invalid partition table signature, which is %x\n", block->name, pt->signature);
        else
            printk("%s: Invalid extended partition table in sector %d\n",
                block->name, sector);
        kfree(pt);
        return;
    }

    /* Parse partitions. */
    for (i = 0; i < sizeof pt->partitions / sizeof *pt->partitions; i++)
    {
        struct partition_table_entry *e = &pt->partitions[i];

        if (e->size == 0 || e->type == 0)
        {
#ifdef DEBUG
            printk("empty partition\n");
#endif
            /* Ignore empty partition. */
        }
        else if (e->type == 0x05       /* Extended partition. */
            || e->type == 0x0f    /* Windows 98 extended partition. */
            || e->type == 0x85    /* Linux extended partition. */
            || e->type == 0xc5)   /* DR-DOS extended partition. */
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
                read_partition_table(block, e->offset, e->offset, part_nr);
            else
                read_partition_table(block, e->offset + primary_extended_sector,
                    primary_extended_sector, part_nr);
        }
        else
        {
            ++*part_nr;

            found_partition(block, e->type, e->offset + sector,
                e->size, *part_nr, e->bootable);
        }
    }

    if (!*part_nr)
    {
#ifdef DEBUG
        printk("use default\n");
#endif
        // no partition table info, assume this disk only has one raw partition
        found_partition(block, 0x21, sector, block->sector_size, 0, 1);

    }
    kfree(pt);
}

#if HDD_CACHE_OPEN
static unsigned total_read = 0;
static unsigned total_writ = 0;
static unsigned cache_hit = 0;
void report_cache()
{
    printk("cache: %d hit, read hdd %d, write hdd %d\n", cache_hit, total_read, total_writ);
    total_writ = total_read = cache_hit = 0;
}

static void init_partition_cache(partition* p)
{
    int i = 0;

    p->cache.count = 0;
    p->cache.hash = hash_create(int_comp);
    InitializeListHead(&p->cache.timer_list_head);
    p->cache_inited = 1;
}



static block_cache_item* hdd_cache_lookup(partition* p, int sector)
{
    int i = 0;
    key_value_pair* pair = 0;
    block_cache_item* ret = 0;
    if (sector == -1)
    {
        return -1;
    }

    pair = hash_find(p->cache.hash, sector);
    if (!pair)
        return 0;

    ret = pair->val;
    cache_hit++;

    return ret;
}

static block_cache_item* hdd_cache_find_empty(partition* p, int sector)
{
    block_cache_item* item;
    if (p->cache.count >= CACHE_SECTOR_COUNT)
    {
        return 0;
    }

    item = block_cache_item_create();
    item->sector = sector;
    InsertTailList(&p->cache.timer_list_head, &item->time_list);
    hash_insert(p->cache.hash, sector, item);
    p->cache.count++;
    return item;
}

static block_cache_item* hdd_cache_find_oldest(partition* p)
{
    LIST_ENTRY* node;
    block_cache_item* item = 0;

    if (p->cache.count == 0)
        return 0;

    node = p->cache.timer_list_head.Flink;
    item = CONTAINER_OF(node, block_cache_item, time_list);
    return item;

}

static void hdd_cache_flush(partition*p, block_cache_item* item)
{
    void* buf = item->buf;
    int sector = item->sector;

    if (sector < 0)
        return;

    if (item->dirty)
    {
        total_writ++;
        partition_write(p, sector, buf, BLOCK_SECTOR_SIZE);
        item->dirty = 0;
    }
}

static void hdd_cache_update(partition*p, block_cache_item* item, int sector, void* buf, int mark_dirty)
{
    int same_sector = (item->sector == sector);

    if (!same_sector)
    {
        hash_remove(p->cache.hash, item->sector);
        hash_insert(p->cache.hash, sector, item);
    }
    item->dirty = mark_dirty;
    item->sector = sector;
    item->last_time = time(0);

    RemoveEntryList(&item->time_list);
    InsertTailList(&p->cache.timer_list_head, &item->time_list);

    memcpy(item->buf, buf, BLOCK_SECTOR_SIZE);
#if HDD_CACHE_WRITE_POLICY == HDD_CACHE_WRITE_THOUGH
    if (mark_dirty)
    {
        hdd_cache_flush(p, item);
    }
#endif

}

static void flush_partition_cache(partition* p)
{
    LIST_ENTRY* entry = p->cache.timer_list_head.Flink;
    LIST_ENTRY* next;
    while (entry != &p->cache.timer_list_head)
    {
        next = entry->Flink;
        block_cache_item* item = CONTAINER_OF(entry, block_cache_item, time_list);
        if (item && item->sector != -1 && item->dirty)
        {
            hdd_cache_flush(p, item);
        }
        block_cache_item_remove(item);
        p->cache.count--;
        entry = next;
    }
}

static int partition_cache_read(void* aux, unsigned sector, void* buf, unsigned len)
{
    partition* p = aux;
    int index = 0;
    block_cache_item* item = 0;

    item = hdd_cache_lookup(p, sector);
    if (item)
    {
        item->last_time = time(0);
        RemoveEntryList(&item->time_list);
        InsertTailList(&p->cache.timer_list_head, &item->time_list);
        memcpy(buf, item->buf, BLOCK_SECTOR_SIZE);
        return BLOCK_SECTOR_SIZE;
    }

    total_read++;
    partition_read(aux, sector, buf, len);
    item = hdd_cache_find_empty(p, sector);
    if (item)
    {
        hdd_cache_update(p, item, sector, buf, 0);
        return len;
    }

    item = hdd_cache_find_oldest(p);
    if (item)
    {
        hdd_cache_flush(p, item);
        hdd_cache_update(p, item, sector, buf, 0);
        return len;
    }

    return -1;

}

static int partition_cache_write(void* aux, unsigned sector, void* buf, unsigned len)
{
    partition* p = aux;
    block_cache_item* item = 0;
    item = hdd_cache_lookup(p, sector);
    if (item)
    {
        hdd_cache_update(p, item, sector, buf, 1);
        return BLOCK_SECTOR_SIZE;
    }

    item = hdd_cache_find_empty(p, sector);
    if (item)
    {
        hdd_cache_update(p, item, sector, buf, 1);
        return len;
    }

    item = hdd_cache_find_oldest(p);
    if (item)
    {
        hdd_cache_flush(p, item);
        hdd_cache_update(p, item, sector, buf, 1);
        return len;
    }

    return -1;
}
#else
void report_cache()
{
}
#endif

static void found_partition(block *block, unsigned char part_type,
    unsigned int start, unsigned int size,
    int part_nr, unsigned char bootable)
{
    if (start >= block->sector_size)
        printk("%s%d: Partition starts past end of device (sector %d)\n",
            block->name, part_nr, start);
    else if (start + size < start || start + size > block->sector_size)
        printk("%s%d: Partition end (%d) past end of device (%d)\n",
            block->name, part_nr, start + size, block->sector_size);
    else
    {
        block_type type = (part_type == 0x20 ? BLOCK_KERNEL
            : part_type == 0x21 ? BLOCK_FILESYS
            : part_type == 0x83 ? BLOCK_LINUX
            : part_type == 0x22 ? BLOCK_SCRATCH
            : part_type == 0x23 ? BLOCK_SWAP
            : BLOCK_UNKNOW);
        partition *p;
        struct _block *b;
        char extra_info[128];
        char name[16];
        char ext[2];
        unsigned size = sizeof(*p) / PAGE_SIZE + 1;
        p = (partition*)vm_alloc(size);
        if (p == 0)
        {
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
        printk("got a partition, with start %d, bootable %x\n", p->start, p->bootable);
#endif

#if HDD_CACHE_OPEN
        b = block_register(p, name, partition_cache_read, partition_cache_write, partition_close, type, size);
#else
        b = block_register(p, name, partition_read, partition_write, partition_close, type, size);
#endif

        if (type == BLOCK_LINUX)
        {
            ext2_attach(b);
        }
        else if (type == BLOCK_FILESYS)
        {
            ffs_attach(b);
        }
    }
}

static void
wait_until_idle(const ata_disk *d)
{
    int i;

    for (i = 0; i < 1000; i++)
    {
        if ((_read_port(reg_status(d->channel)) & (STA_BSY | STA_DRQ)) == 0)
            return;
        delay(10);
    }

    printk("%s: idle timeout\n", d->name);
}

static void select_device_wait(const ata_disk * d)
{
    channel* c = d->channel;
    //printk("[hdd] sema reset (to 1)\n");
    sema_reset(&c->sema);
    c->expecting_interrupt = 1;
    //wait_until_idle (d);
    select_device(d);
    //wait_until_idle (d);
}

static void issue_pio_command(channel * c, unsigned char command)
{

    _write_port(reg_command(c), command);
}

static void input_sector(channel * c, void * sector)
{
    _read_dwb(reg_data(c), sector, BLOCK_SECTOR_SIZE / 4);
}

static void output_sector(channel *c, const void *sector)
{

    _write_dwb(reg_data(c), sector, BLOCK_SECTOR_SIZE / 4);
}


static char *descramble_ata_string(char * string, int size)
{
    int i;

    /* Swap all pairs of bytes. */
    for (i = 0; i + 1 < size; i += 2)
    {
        char tmp = string[i];
        string[i] = string[i + 1];
        string[i + 1] = tmp;
    }

    /* Find the last non-white, non-null character. */
    for (size--; size > 0; size--)
    {
        int c = string[size - 1];
        if (c != '\0' && !isspace(c))
            break;
    }
    string[size] = '\0';

    return string;
}

static void
select_sector(ata_disk *d, unsigned int sec_no)
{
    channel *c = d->channel;


    select_device_wait(d);
    _write_port(reg_nsect(c), 1);
    _write_port(reg_lbal(c), sec_no);
    _write_port(reg_lbam(c), sec_no >> 8);
    _write_port(reg_lbah(c), (sec_no >> 16));
    _write_port(reg_device(c),
        DEV_MBS | DEV_LBA | (d->dev_no == 1 ? DEV_DEV : 0) | (sec_no >> 24));
}


#ifdef DEBUG_FFS
static time_t read_select, read_wait, read_io;
static time_t writ_select, writ_wait, writ_io;

static unsigned long read_select_t = 0, read_wait_t = 0, read_io_t = 0;
static unsigned long writ_select_t = 0, writ_wait_t = 0, writ_io_t = 0;

static unsigned long read_times = 0, write_times = 0;

static void break_hdd_read_select_begin()
{
    timer_current(&read_select);
}

static void break_hdd_read_select_end()
{
    time_t now;
    timer_current(&now);

    read_select_t += (now.seconds * 1000 + now.milliseconds -
        read_select.seconds * 1000 - read_select.milliseconds);
}

static void break_hdd_read_wait_begin()
{
    timer_current(&read_wait);
}

static void break_hdd_read_wait_end()
{
    time_t now;
    timer_current(&now);

    read_wait_t += (now.seconds * 1000 + now.milliseconds -
        read_wait.seconds * 1000 - read_wait.milliseconds);
}

static void break_hdd_read_io_begin()
{
    timer_current(&read_io);
}

static void break_hdd_read_io_end()
{
    time_t now;
    timer_current(&now);

    read_io_t += (now.seconds * 1000 + now.milliseconds -
        read_io.seconds * 1000 - read_io.milliseconds);
}

static void break_hdd_write_select_begin()
{
    timer_current(&writ_select);
}

static void break_hdd_write_select_end()
{
    time_t now;
    timer_current(&now);

    writ_select_t += (now.seconds * 1000 + now.milliseconds -
        writ_select.seconds * 1000 - writ_select.milliseconds);
}

static void break_hdd_write_wait_begin()
{
    timer_current(&writ_wait);
}

static void break_hdd_write_wait_end()
{
    time_t now;
    timer_current(&now);

    writ_wait_t += (now.seconds * 1000 + now.milliseconds -
        writ_wait.seconds * 1000 - writ_wait.milliseconds);

}

static void break_hdd_write_io_begin()
{
    timer_current(&writ_io);
}

static void break_hdd_write_io_end()
{
    time_t now;
    timer_current(&now);

    writ_io_t += (now.seconds * 1000 + now.milliseconds -
        writ_io.seconds * 1000 - writ_io.milliseconds);

}

void report_hdd_time()
{
    printk("read  %d times: select %u, wait %u, io %u\n", read_times, read_select_t,
        read_wait_t, read_io_t);
    printk("write %d times: select %u, wait %u, io %u\n", write_times, writ_select_t,
        writ_wait_t, writ_io_t);
    read_select_t = 0, read_wait_t = 0, read_io_t = 0;
    writ_select_t = 0, writ_wait_t = 0, writ_io_t = 0;
    read_times = write_times = 0;
}
#endif

static int hdd_read(void* aux, unsigned sec_no, void* buf, unsigned len)
{
    ata_disk *d = aux;
    channel *c = d->channel;
    sema_wait_for_intr(&c->iolock);
#ifdef DEBUG_FFS
    break_hdd_read_select_begin();
#endif
    select_sector(d, sec_no);
#ifdef DEBUG_FFS
    break_hdd_read_select_end();
#endif

#ifdef DEBUG_FFS
    break_hdd_read_wait_begin();
#endif
    issue_pio_command(c, CMD_READ_SECTOR_RETRY);
    //printk("[hdd] read  sema wait <<\n");
    sema_wait_for_intr(&c->sema);
    //printk("[hdd] read  sema wait >>\n");
  //if (!wait_while_busy (d)){
  //  printk ("%s: disk read failed, sector=%d\n", d->name, sec_no);
  //  return 0;
  //}
#ifdef DEBUG_FFS
    break_hdd_read_wait_end();
#endif

#ifdef DEBUG_FFS
    break_hdd_read_io_begin();
#endif
    input_sector(c, buf);
#ifdef DEBUG_FFS
    break_hdd_read_io_end();
    read_times++;

#endif
    sema_trigger(&c->iolock);

    return len;
}


static int hdd_write(void* aux, unsigned sec_no, void* buf, unsigned len)
{
    ata_disk *d = aux;
    channel *c = d->channel;
    sema_wait_for_intr(&c->iolock);
#ifdef DEBUG_FFS
    break_hdd_write_select_begin();
#endif
    select_sector(d, sec_no);
#ifdef DEBUG_FFS
    break_hdd_write_select_end();
#endif
    issue_pio_command(c, CMD_WRITE_SECTOR_RETRY);
    //if (!wait_while_busy (d)){
    //  printk ("%s: disk write failed, sector=%d\n", d->name, sec_no);
    //  return 0;
    //}
#ifdef DEBUG_FFS
    break_hdd_write_io_begin();
#endif
    output_sector(c, buf);
#ifdef DEBUG_FFS
    break_hdd_write_io_end();
#endif

#ifdef DEBUG_FFS
    break_hdd_write_wait_begin();
#endif
    //printk("[hdd] write sema wait <<\n");
    sema_wait_for_intr(&c->sema);
    //printk("[hdd] write sema wait >>\n");
#ifdef DEBUG_FFS
    break_hdd_write_wait_end();
    write_times++;

#endif
    sema_trigger(&c->iolock);

    return len;
}

static void hdd_close(void* aux)
{

}


static int partition_read(void* aux, unsigned sector, void* buf, unsigned len)
{
    partition *p = aux;
    return p->block->read(p->block->aux, p->start + sector, buf, len);

}

static int partition_write(void* aux, unsigned sector, void* buf, unsigned len)
{
    partition *p = aux;
    return p->block->write(p->block->aux, p->start + sector, buf, len);

}

static void partition_close(void* aux)
{
#if HDD_CACHE_OPEN
    int i = 0;
    partition* p = aux;
    if (p->cache_inited)
    {
        flush_partition_cache(aux);
        p->cache_inited = 0;
    }
#endif
}


