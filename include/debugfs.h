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

file *debugfs_open(debug_inode *inode);

void debugfs_mm_init(super_block *sb);

void debugfs_ps_init(super_block *sb);

void debugfs_cpu_init(super_block *sb);

void debugfs_sched_init(super_block *sb);

void debugfs_fs_init(super_block *sb);

#endif