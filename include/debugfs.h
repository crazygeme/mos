#ifndef _DEBUGFS_H
#define _DEBUGFS_H

#include <mount.h>

#define DBGFS_INODE 0x80

typedef struct _debug_inode {
	void (*fill)(void *buf, size_t size);
	void *buf;
	unsigned len;
	unsigned offset;
} debug_inode;

void debugfs_init();

file * debugfs_open(debug_inode *inode);

void debugfs_mm_init(mount_point *mp);

void debugfs_ps_init(mount_point *mp);

void debugfs_cpu_init(mount_point *mp);

void debugfs_sched_init(mount_point *mp);

void debugfs_fs_init(mount_point *mp);

#endif