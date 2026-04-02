#ifndef _DEV_LOOPDEV_H_
#define _DEV_LOOPDEV_H_

#include <stdint.h>

#define LOOP_MAX_DEVS 8

/*
 * loop_dev_info — public descriptor for one loop device slot.
 *
 * name[] is empty when the slot is free.
 * Populated by loop_setup(), cleared by loop_teardown().
 * Consumed by /proc/partitions.
 */
typedef struct {
	char name[16]; /* "loop0" .. "loop7"; empty = free */
	char backing[256]; /* absolute path of backing file */
	uint64_t size_bytes;
} loop_dev_info;

extern loop_dev_info loop_devs[LOOP_MAX_DEVS];

/*
 * loop_setup — associate a free loop slot with the file at @path.
 * Allocates a bdev backed by file I/O and registers it with lwext4.
 * Returns the device name (e.g. "loop0") on success, NULL on failure.
 */
const char *loop_setup(const char *path);

/*
 * loop_teardown — detach a loop device by its name.
 * Unregisters the bdev from lwext4 and closes the backing file.
 * Must not be called while the device is still mounted.
 */
void loop_teardown(const char *name);

#endif
