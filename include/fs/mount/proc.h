#ifndef _FS_MOUNT_PROC_H
#define _FS_MOUNT_PROC_H
#include <fs/mount.h>

#define DBGFS_INODE 0x80

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