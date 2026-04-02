#ifndef _DEV_DEV_H
#define _DEV_DEV_H

#include <fs/vfs.h>
#include <fs/fs.h>

#define DEV_INODE 0x90

/* Device number encoding (Linux-compatible 8-bit major/minor). */
#define MKDEV(major, minor) \
	(((unsigned)(major) << 8) | ((unsigned)(minor)&0xFF))
#define MAJOR(dev) ((unsigned)(dev) >> 8)
#define MINOR(dev) ((unsigned)(dev)&0xFF)

/*
 * cdev_register - register a character or block device handler.
 *
 * mode_type:   S_IFCHR or S_IFBLK
 * major:       device major number
 * minor_base:  first minor handled
 * minor_count: number of consecutive minors handled
 * open:        called on open(); rdev = MKDEV(major, minor)
 *              returns a new open file* on success, NULL on error.
 */
void cdev_register(unsigned mode_type, unsigned major, unsigned minor_base,
		   unsigned minor_count,
		   file *(*open)(unsigned rdev, int flag));

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

#define DEV_INIT(fn)                           \
	static dev_init_fn_t __devfs_init_##fn \
		__attribute__((used, section(".devfs_init"))) = (fn)

#endif /* _DEV_DEV_H */
