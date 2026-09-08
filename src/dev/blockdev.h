#ifndef _DEV_BLOCKDEV_H
#define _DEV_BLOCKDEV_H

#include <stdint.h>

#define BLOCKDEV_FLAG_MOUNTABLE 0x0001

typedef struct {
	char name[32];
	unsigned major;
	unsigned minor;
	uint64_t size_bytes;
	unsigned flags;
} blockdev_info;

typedef void (*blockdev_iter_fn)(const blockdev_info *info, void *data);

void blockdev_register(const char *name, unsigned major, unsigned minor,
		       uint64_t size_bytes, unsigned flags);
void blockdev_update(const char *name, uint64_t size_bytes, unsigned flags);
int blockdev_lookup(const char *name, blockdev_info *out);
int blockdev_lookup_mountable(const char *name, blockdev_info *out);
int blockdev_first_mountable(blockdev_info *out);
void blockdev_for_each(blockdev_iter_fn fn, void *data);

#endif
