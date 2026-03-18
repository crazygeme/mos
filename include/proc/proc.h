#ifndef _PROC_PROC_H
#define _PROC_PROC_H

#include <fs/vfs.h>

#define PROC_INODE 0x80

/*
 * PROC_INIT(fn) registers a void (*)(super_block *) function to be called
 * during procfs initialisation.  All static /proc entries use this to
 * self-register without central coordination — analogous to Linux's
 * __initcall mechanism.
 *
 * Usage:
 *   static void my_proc_init(super_block *proc_sb) {
 *       vfs_mount(proc_sb, "/myfile", sget(&my_sops));
 *   }
 *   PROC_INIT(my_proc_init);
 */
typedef void (*proc_init_fn_t)(super_block *);
extern proc_init_fn_t __procfs_init_start[];
extern proc_init_fn_t __procfs_init_end[];

#define PROC_INIT(fn)                            \
	static proc_init_fn_t __procfs_init_##fn \
		__attribute__((used, section(".procfs_init"))) = (fn)

#endif /* _PROC_PROC_H */
