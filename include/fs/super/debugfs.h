#ifndef _FS_SUPER_DEBUGFS_H
#define _FS_SUPER_DEBUGFS_H

#include <fs/mount.h>

#define DBGFS_INODE 0x80

typedef struct _debug_inode {
	void (*fill)(void *buf, size_t size);
	void *buf;
	unsigned len;
	unsigned offset;
} debug_inode;

file *debugfs_open(debug_inode *inode);

/*
 * DEBUGFS_INIT(fn) registers a void (*)(super_block *) function to be called
 * during debugfs initialisation.  All entries share the same ".debugfs_init"
 * section; ordering is not guaranteed and not required.
 */
typedef void (*debugfs_init_fn_t)(super_block *);
extern debugfs_init_fn_t __debugfs_init_start[];
extern debugfs_init_fn_t __debugfs_init_end[];

#define DEBUGFS_INIT(fn)                             \
	static debugfs_init_fn_t __debugfs_init_##fn \
		__attribute__((used, section(".debugfs_init"))) = (fn)

#endif