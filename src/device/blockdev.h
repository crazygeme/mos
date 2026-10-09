#ifndef MOS_DEVICE_BLOCKDEV_H
#define MOS_DEVICE_BLOCKDEV_H

#include <stdint.h>
#include <device/devnode.h>

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
int blockdev_update(const char *name, uint64_t size_bytes, unsigned flags);
int blockdev_lookup(const char *name, blockdev_info *out);
int blockdev_lookup_mountable(const char *name, blockdev_info *out);
int blockdev_first_mountable(blockdev_info *out);
void blockdev_for_each(blockdev_iter_fn fn, void *data);

/* Storage backends register lifecycle and cache services at probe time. */
typedef struct {
	uint64_t cached_bytes, peak_bytes, read_bytes, write_bytes;
	unsigned hits, searches;
	uint64_t physical_read_bytes, physical_write_bytes;
} blockdev_stats;
typedef struct {
	void (*flush)(void);
	void (*close)(void);
	unsigned (*reclaim)(unsigned pages);
	void (*stats)(blockdev_stats *stats);
	const char *(*attach_file)(const char *path);
	int (*detach_file)(const char *name);
} blockdev_backend;
void blockdev_register_backend(const blockdev_backend *backend);
void blockdev_flush_all(void);
void blockdev_close_all(void);
unsigned blockdev_reclaim(unsigned pages);
void blockdev_get_stats(blockdev_stats *stats);
unsigned blockdev_cached_pages(void);

/* File-backed block devices are managed through the block subsystem. */
const char *blockdev_attach_file(const char *path);
int blockdev_detach_file(const char *name);
/* Sector I/O returns zero on a complete transfer or a negative errno. */
typedef struct {
	int (*read)(void *data, void *buffer, uint64_t sector, unsigned count);
	int (*write)(void *data, const void *buffer, uint64_t sector,
		     unsigned count);
} blockdev_io;
typedef struct blockdev_handle blockdev_handle;
int blockdev_bind_io(const char *name, unsigned sector_size,
		     const blockdev_io *ops, void *data);
int blockdev_unbind_io(const char *name);
blockdev_handle *blockdev_open(const char *name);
void blockdev_close(blockdev_handle *device);
unsigned blockdev_sector_size(const blockdev_handle *device);
uint64_t blockdev_sector_count(const blockdev_handle *device);
int blockdev_read(blockdev_handle *device, void *buffer, uint64_t sector,
		  unsigned count);
int blockdev_write(blockdev_handle *device, const void *buffer, uint64_t sector,
		   unsigned count);
/* Block-special files dispatch through the block subsystem. */
int blockdev_register_node(unsigned devno, const char *name,
			   device_node_open_fn open);
void blockdev_for_each_class(device_number_iter_fn fn, void *data);
file *blockdev_open_node(super_block *sb, unsigned devno, int flags,
			 int *matched);
#endif
