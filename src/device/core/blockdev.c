#include <device/blockdev.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <errno.h>
#include <fs/mount.h>

#define MAX_BLOCKDEVS 32

struct blockdev_handle {
	int used, removed;
	blockdev_info info;
	const blockdev_io *ops;
	void *data;
	unsigned sector_size, references;
};
typedef struct blockdev_handle blockdev_slot;
static spinlock_t blockdev_io_lock = SPINLOCK_INITIALIZER;

static blockdev_slot blockdev_table[MAX_BLOCKDEVS];

static const char *blockdev_canon(const char *name)
{
	if (!name)
		return NULL;
	return (strncmp(name, "/dev/", 5) == 0) ? name + 5 : name;
}

static blockdev_slot *blockdev_find_slot(const char *name)
{
	const char *canon = blockdev_canon(name);
	int i;

	if (!canon || !*canon)
		return NULL;

	for (i = 0; i < MAX_BLOCKDEVS; i++) {
		if (blockdev_table[i].used && !blockdev_table[i].removed &&
		    strcmp(blockdev_table[i].info.name, canon) == 0)
			return &blockdev_table[i];
	}
	return NULL;
}

int blockdev_present(const blockdev_handle *device)
{
	int irq, present;
	spinlock_lock(&blockdev_io_lock, &irq);
	present = device && device->used && !device->removed;
	spinlock_unlock(&blockdev_io_lock, irq);
	return present;
}

void blockdev_register(const char *name, unsigned major, unsigned minor,
		       uint64_t size_bytes, unsigned flags)
{
	blockdev_slot *slot;
	int i;

	if (!name || !*name)
		return;
	int irq;
	spinlock_lock(&blockdev_io_lock, &irq);
	slot = blockdev_find_slot(name);
	if (!slot) {
		for (i = 0; i < MAX_BLOCKDEVS; i++) {
			if (!blockdev_table[i].used) {
				slot = &blockdev_table[i];
				slot->used = 1;
				break;
			}
		}
	}
	if (!slot) {
		spinlock_unlock(&blockdev_io_lock, irq);
		return;
	}

	memset(&slot->info, 0, sizeof(slot->info));
	strncpy(slot->info.name, blockdev_canon(name),
		sizeof(slot->info.name) - 1);
	slot->info.major = major;
	slot->info.minor = minor;
	slot->info.size_bytes = size_bytes;
	slot->info.flags = flags;
	spinlock_unlock(&blockdev_io_lock, irq);
}

int blockdev_update(const char *name, uint64_t size_bytes, unsigned flags)
{
	blockdev_slot *slot;
	int irq, error = 0;
	spinlock_lock(&blockdev_io_lock, &irq);
	slot = blockdev_find_slot(name);
	if (!slot)
		error = -ENODEV;
	else if (slot->references && (size_bytes != slot->info.size_bytes ||
				      flags != slot->info.flags))
		error = -EBUSY;
	else {
		slot->info.size_bytes = size_bytes;
		slot->info.flags = flags;
	}
	spinlock_unlock(&blockdev_io_lock, irq);
	return error;
}

static int blockdev_lookup_flags(const char *name, blockdev_info *out,
				 unsigned required)
{
	int irq, found = 0;
	spinlock_lock(&blockdev_io_lock, &irq);
	blockdev_slot *slot = blockdev_find_slot(name);
	if (slot && (slot->info.flags & required) == required) {
		if (out)
			*out = slot->info;
		found = 1;
	}
	spinlock_unlock(&blockdev_io_lock, irq);
	return found;
}
int blockdev_lookup(const char *name, blockdev_info *out)
{
	return blockdev_lookup_flags(name, out, 0);
}
int blockdev_lookup_mountable(const char *name, blockdev_info *out)
{
	blockdev_info info;
	if (!blockdev_lookup_flags(name, &info, BLOCKDEV_FLAG_VOLUME))
		return 0;
	blockdev_handle *device = blockdev_open(name);
	if (!device)
		return 0;
	int found = fs_probe_block(device) != NULL;
	blockdev_close(device);
	if (found && out)
		*out = info;
	return found;
}
int blockdev_first_mountable(blockdev_info *out)
{
	for (unsigned i = 0; i < MAX_BLOCKDEVS; i++) {
		blockdev_info info;
		int irq, present;
		spinlock_lock(&blockdev_io_lock, &irq);
		present = blockdev_table[i].used && !blockdev_table[i].removed;
		if (present)
			info = blockdev_table[i].info;
		spinlock_unlock(&blockdev_io_lock, irq);
		if (present && blockdev_lookup_mountable(info.name, out))
			return 1;
	}
	return 0;
}
void blockdev_for_each(blockdev_iter_fn fn, void *data)
{
	if (!fn)
		return;
	for (unsigned i = 0; i < MAX_BLOCKDEVS; i++) {
		blockdev_info snapshot;
		int irq, present;
		spinlock_lock(&blockdev_io_lock, &irq);
		present = blockdev_table[i].used && !blockdev_table[i].removed;
		if (present)
			snapshot = blockdev_table[i].info;
		spinlock_unlock(&blockdev_io_lock, irq);
		if (present)
			fn(&snapshot, data);
	}
}

#define MAX_BLOCK_BACKENDS 8
static const blockdev_backend *backends[MAX_BLOCK_BACKENDS];
static unsigned backend_count;

void blockdev_register_backend(const blockdev_backend *backend)
{
	if (!backend)
		return;
	for (unsigned i = 0; i < backend_count; i++)
		if (backends[i] == backend)
			return;
	if (backend_count < MAX_BLOCK_BACKENDS)
		backends[backend_count++] = backend;
}

void blockdev_flush_all(void)
{
	for (unsigned i = 0; i < backend_count; i++)
		if (backends[i]->flush)
			backends[i]->flush();
}

void blockdev_close_all(void)
{
	for (unsigned i = 0; i < backend_count; i++)
		if (backends[i]->close)
			backends[i]->close();
}

unsigned blockdev_reclaim(unsigned pages)
{
	unsigned reclaimed = 0;
	for (unsigned i = 0; i < backend_count && reclaimed < pages; i++)
		if (backends[i]->reclaim)
			reclaimed += backends[i]->reclaim(pages - reclaimed);
	return reclaimed;
}

void blockdev_get_stats(blockdev_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	for (unsigned i = 0; i < backend_count; i++) {
		blockdev_stats sample = { 0 };
		if (!backends[i]->stats)
			continue;
		backends[i]->stats(&sample);
		stats->cached_bytes += sample.cached_bytes;
		stats->peak_bytes += sample.peak_bytes;
		stats->read_bytes += sample.read_bytes;
		stats->write_bytes += sample.write_bytes;
		stats->hits += sample.hits;
		stats->searches += sample.searches;
		stats->physical_read_bytes += sample.physical_read_bytes;
		stats->physical_write_bytes += sample.physical_write_bytes;
	}
}

unsigned blockdev_cached_pages(void)
{
	blockdev_stats stats;
	blockdev_get_stats(&stats);
	return stats.cached_bytes / PAGE_SIZE;
}

int blockdev_bind_io(const char *name, unsigned sector_size,
		     const blockdev_io *ops, void *data)
{
	int irq, error = 0;
	blockdev_slot *slot;
	if (!sector_size || !ops || !ops->read)
		return -EINVAL;
	spinlock_lock(&blockdev_io_lock, &irq);
	slot = blockdev_find_slot(name);
	if (!slot)
		error = -ENODEV;
	else if (slot->references)
		error = -EBUSY;
	else {
		slot->ops = ops;
		slot->data = data;
		slot->sector_size = sector_size;
	}
	spinlock_unlock(&blockdev_io_lock, irq);
	return error;
}

int blockdev_unbind_io(const char *name)
{
	int irq, error = 0;
	blockdev_slot *slot;
	spinlock_lock(&blockdev_io_lock, &irq);
	slot = blockdev_find_slot(name);
	if (!slot)
		error = -ENODEV;
	else if (slot->references)
		error = -EBUSY;
	else {
		slot->ops = NULL;
		slot->data = NULL;
	}
	spinlock_unlock(&blockdev_io_lock, irq);
	return error;
}

blockdev_handle *blockdev_open(const char *name)
{
	int irq;
	blockdev_slot *slot;
	spinlock_lock(&blockdev_io_lock, &irq);
	slot = blockdev_find_slot(name);
	if (!slot || !slot->ops || !(slot->info.flags & BLOCKDEV_FLAG_VOLUME))
		slot = NULL;
	else
		slot->references++;
	spinlock_unlock(&blockdev_io_lock, irq);
	return slot;
}

/* Retired slots remain distinct while an old filesystem retains a handle. */
void blockdev_unregister(const char *name)
{
	void (*release)(void *) = NULL;
	void *data = NULL;
	int irq;
	spinlock_lock(&blockdev_io_lock, &irq);
	blockdev_slot *slot = blockdev_find_slot(name);
	if (slot) {
		slot->removed = 1;
		slot->info.flags = 0;
		if (!slot->references) {
			release = slot->ops ? slot->ops->release : NULL;
			data = slot->data;
			memset(slot, 0, sizeof(*slot));
		}
	}
	spinlock_unlock(&blockdev_io_lock, irq);
	if (release)
		release(data);
}
void blockdev_close(blockdev_handle *device)
{
	void (*release)(void *) = NULL;
	void *data = NULL;
	int irq;
	if (!device)
		return;
	spinlock_lock(&blockdev_io_lock, &irq);
	if (device->references)
		device->references--;
	if (!device->references && device->removed) {
		release = device->ops ? device->ops->release : NULL;
		data = device->data;
		memset(device, 0, sizeof(*device));
	}
	spinlock_unlock(&blockdev_io_lock, irq);
	if (release)
		release(data);
}

unsigned blockdev_sector_size(const blockdev_handle *device)
{
	return device ? device->sector_size : 0;
}

uint64_t blockdev_sector_count(const blockdev_handle *device)
{
	return device && device->sector_size ?
		       device->info.size_bytes / device->sector_size :
		       0;
}

static int blockdev_check_range(blockdev_handle *device, uint64_t sector,
				unsigned count)
{
	uint64_t capacity = blockdev_sector_count(device);
	if (!device || device->removed || !device->ops || !device->references)
		return -ENODEV;
	if (sector > capacity || count > capacity - sector ||
	    count > ~0U / device->sector_size)
		return -EIO;
	return 0;
}

int blockdev_read(blockdev_handle *device, void *buffer, uint64_t sector,
		  unsigned count)
{
	int error = blockdev_check_range(device, sector, count);
	return error || !count ?
		       error :
		       device->ops->read(device->data, buffer, sector, count);
}

int blockdev_write(blockdev_handle *device, const void *buffer, uint64_t sector,
		   unsigned count)
{
	int error = blockdev_check_range(device, sector, count);
	if (error || !count)
		return error;
	return device->ops->write ?
		       device->ops->write(device->data, buffer, sector, count) :
		       -EROFS;
}

#define MAX_ENDPOINTS 256

typedef struct block_endpoint_entry {
	unsigned mode_type; /* S_IFCHR or S_IFBLK */
	unsigned major;
	unsigned minor_base;
	unsigned minor_count;
	const char *name;
	file *(*open)(super_block *sb, unsigned rdev, int flag);
	struct rb_node major_node;
	list_entry major_ranges;
} block_endpoint_entry;

static block_endpoint_entry block_endpoint_table[MAX_ENDPOINTS];
static int block_endpoint_count;
static struct rb_root block_endpoint_majors = _RBTREE_ROOT_INIT;

static int block_endpoint_compare(unsigned mode, unsigned major,
				  const block_endpoint_entry *entry)
{
	if (mode != entry->mode_type)
		return mode < entry->mode_type ? -1 : 1;
	return major < entry->major ? -1 : major != entry->major;
}

static block_endpoint_entry *block_endpoint_find_major(unsigned mode,
						       unsigned major)
{
	struct rb_node *node = block_endpoint_majors.rb_node;
	while (node) {
		block_endpoint_entry *entry =
			rb_entry(node, block_endpoint_entry, major_node);
		int order = block_endpoint_compare(mode, major, entry);
		if (!order)
			return entry;
		node = order < 0 ? node->rb_left : node->rb_right;
	}
	return NULL;
}

static void
block_endpoint_register(unsigned mode_type, unsigned major, unsigned minor_base,
			unsigned minor_count, const char *name,
			file *(*open)(super_block *sb, unsigned rdev, int flag))
{
	struct rb_node **link = &block_endpoint_majors.rb_node, *parent = NULL;
	block_endpoint_entry *entry, *head = NULL;
	if (block_endpoint_count >= MAX_ENDPOINTS)
		return;
	entry = &block_endpoint_table[block_endpoint_count++];
	entry->mode_type = mode_type;
	entry->major = major;
	entry->minor_base = minor_base;
	entry->minor_count = minor_count;
	entry->name = name;
	entry->open = open;
	while (*link) {
		block_endpoint_entry *existing =
			rb_entry(*link, block_endpoint_entry, major_node);
		int order = block_endpoint_compare(mode_type, major, existing);
		if (!order) {
			head = existing;
			break;
		}
		parent = *link;
		link = order < 0 ? &parent->rb_left : &parent->rb_right;
	}
	list_init(&entry->major_ranges);
	if (head) {
		list_insert_tail(&head->major_ranges, &entry->major_ranges);
		RB_CLEAR_NODE(&entry->major_node);
		return;
	}
	rb_init_node(&entry->major_node);
	rb_link_node(&entry->major_node, parent, link);
	rb_insert_color(&entry->major_node, &block_endpoint_majors);
}

void blockdev_for_each_class(device_number_iter_fn fn, void *data)
{
	int i;
	if (!fn)
		return;
	for (i = 0; i < block_endpoint_count; i++)
		if (!RB_EMPTY_NODE(&block_endpoint_table[i].major_node))
			fn(block_endpoint_table[i].mode_type,
			   block_endpoint_table[i].major,
			   block_endpoint_table[i].name, data);
}

static file *block_endpoint_open(super_block *sb, unsigned mode, unsigned devno,
				 int flags, int *matched)
{
	block_endpoint_entry *head = block_endpoint_find_major(mode & S_IFMT,
							       MAJOR(devno)),
			     *entry;
	*matched = 0;
	if (!head)
		return NULL;
	entry = head;
	do {
		unsigned minor = MINOR(devno);
		if (entry->open && minor >= entry->minor_base &&
		    minor - entry->minor_base < entry->minor_count) {
			*matched = 1;
			return entry->open(sb, devno, flags);
		}
		entry = container_of(entry->major_ranges.next,
				     block_endpoint_entry, major_ranges);
	} while (entry != head);
	return NULL;
}

int blockdev_register_node(unsigned devno, const char *name,
			   device_node_open_fn open)
{
	block_endpoint_entry *head = block_endpoint_find_major(S_IFBLK,
							       MAJOR(devno)),
			     *entry = head;
	if (head)
		do {
			if (entry->open && entry->minor_count &&
			    entry->minor_base == MINOR(devno))
				return entry->open == open ? 0 : -EEXIST;
			entry = container_of(entry->major_ranges.next,
					     block_endpoint_entry,
					     major_ranges);
		} while (entry != head);
	if (block_endpoint_count >= MAX_ENDPOINTS)
		return -ENOSPC;
	block_endpoint_register(S_IFBLK, MAJOR(devno), MINOR(devno), 1, name,
				open);
	return 0;
}

file *blockdev_open_node(super_block *sb, unsigned devno, int flags,
			 int *matched)
{
	return block_endpoint_open(sb, S_IFBLK, devno, flags, matched);
}

const char *blockdev_attach_file(const char *path)
{
	for (unsigned i = 0; i < backend_count; i++)
		if (backends[i]->attach_file)
			return backends[i]->attach_file(path);
	return NULL;
}

int blockdev_detach_file(const char *name)
{
	for (unsigned i = 0; i < backend_count; i++)
		if (backends[i]->detach_file) {
			int error = backends[i]->detach_file(name);
			if (error != -ENODEV)
				return error;
		}
	return -ENODEV;
}
