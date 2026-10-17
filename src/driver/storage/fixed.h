#ifndef MOS_STORAGE_FIXED_H
#define MOS_STORAGE_FIXED_H
#include <driver/driver.h>
#include <lib/lock.h>
/* Fixed NVM block devices use native logical sectors and 64-bit LBAs. */
struct fixed_cache;
struct fixed_disk {
	struct fixed_cache *cache;
	void *private;
	uint64_t sectors;
	unsigned sector_size, major, index;
	char name[24];
	uint32_t pci;
	rmutex_t lock;
	int (*rw)(struct fixed_disk *, void *, uint64_t, unsigned, int);
	int (*flush)(struct fixed_disk *);
	void (*shutdown)(struct fixed_disk *);
	int (*discard)(struct fixed_disk *, uint64_t, uint64_t);
};
int fixed_disk_register(struct fixed_disk *disk);
void *storage_map_bar(unsigned pci, unsigned bar, uint64_t minimum);
#endif
