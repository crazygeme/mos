#include <dev/blockdev.h>
#include <lib/klib.h>

#define MAX_BLOCKDEVS 32

typedef struct {
	int used;
	blockdev_info info;
} blockdev_slot;

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
		if (blockdev_table[i].used &&
		    strcmp(blockdev_table[i].info.name, canon) == 0)
			return &blockdev_table[i];
	}
	return NULL;
}

void blockdev_register(const char *name, unsigned major, unsigned minor,
		       uint64_t size_bytes, unsigned flags)
{
	blockdev_slot *slot;
	int i;

	if (!name || !*name)
		return;

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
	if (!slot)
		return;

	memset(&slot->info, 0, sizeof(slot->info));
	strncpy(slot->info.name, blockdev_canon(name),
		sizeof(slot->info.name) - 1);
	slot->info.major = major;
	slot->info.minor = minor;
	slot->info.size_bytes = size_bytes;
	slot->info.flags = flags;
}

void blockdev_update(const char *name, uint64_t size_bytes, unsigned flags)
{
	blockdev_slot *slot = blockdev_find_slot(name);

	if (!slot)
		return;

	slot->info.size_bytes = size_bytes;
	slot->info.flags = flags;
}

int blockdev_lookup(const char *name, blockdev_info *out)
{
	blockdev_slot *slot = blockdev_find_slot(name);

	if (!slot)
		return 0;
	if (out)
		*out = slot->info;
	return 1;
}

int blockdev_lookup_mountable(const char *name, blockdev_info *out)
{
	blockdev_slot *slot = blockdev_find_slot(name);

	if (!slot || !(slot->info.flags & BLOCKDEV_FLAG_MOUNTABLE))
		return 0;
	if (out)
		*out = slot->info;
	return 1;
}

int blockdev_first_mountable(blockdev_info *out)
{
	int i;

	for (i = 0; i < MAX_BLOCKDEVS; i++) {
		if (blockdev_table[i].used &&
		    (blockdev_table[i].info.flags & BLOCKDEV_FLAG_MOUNTABLE)) {
			if (out)
				*out = blockdev_table[i].info;
			return 1;
		}
	}
	return 0;
}

void blockdev_for_each(blockdev_iter_fn fn, void *data)
{
	int i;

	if (!fn)
		return;

	for (i = 0; i < MAX_BLOCKDEVS; i++) {
		if (blockdev_table[i].used)
			fn(&blockdev_table[i].info, data);
	}
}
