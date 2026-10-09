#ifndef MOS_DEVICE_DEVNODE_H
#define MOS_DEVICE_DEVNODE_H

#include <fs/vfs.h>
#include <fs/fs.h>

#define DEV_INODE 0x90

/* Device number encoding (Linux-compatible 8-bit major/minor). */
#define MKDEV(major, minor) \
	(((unsigned)(major) << 8) | ((unsigned)(minor) & 0xFF))
#define MAJOR(dev) ((unsigned)(dev) >> 8)
#define MINOR(dev) ((unsigned)(dev) & 0xFF)

/* Public device number used by VM code to recognize /dev/mem mappings. */
#define DEV_MEM_RDEV MKDEV(1, 1)

typedef file *(*device_node_open_fn)(super_block *sb, unsigned devno,
				     int flags);
typedef void (*device_number_iter_fn)(unsigned mode, unsigned major,
				      const char *name, void *data);

/* Character/block device and FIFO node backend used by VFS entries and mknod. */
super_block *devnode_create(unsigned mode, unsigned devno);

#endif /* MOS_DEVICE_DEVNODE_H */
