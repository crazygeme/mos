#ifndef _DEV_DEV_H
#define _DEV_DEV_H

#include <fs/vfs.h>

#define DEV_INODE 0x90

/*
 * DEV_INIT(fn) registers a void (*)(super_block *) function to be called
 * during devfs initialisation.  All /dev entries use this to self-register
 * without central coordination — analogous to PROC_INIT for /proc.
 *
 * Usage:
 *   static void my_dev_register(super_block *dev_sb) {
 *       vfs_mount(dev_sb, "/mydev", sget(&my_sops));
 *   }
 *   DEV_INIT(my_dev_register);
 */
typedef void (*dev_init_fn_t)(super_block *);
extern dev_init_fn_t __devfs_init_start[];
extern dev_init_fn_t __devfs_init_end[];

#define DEV_INIT(fn)                             \
	static dev_init_fn_t __devfs_init_##fn   \
		__attribute__((used, section(".devfs_init"))) = (fn)

#endif /* _DEV_DEV_H */
