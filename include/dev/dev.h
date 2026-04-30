#ifndef _DEV_DEV_H
#define _DEV_DEV_H

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

/*
 * cdev_register - register a character or block device handler.
 *
 * mode_type:   S_IFCHR or S_IFBLK
 * major:       device major number
 * minor_base:  first minor handled
 * minor_count: number of consecutive minors handled
 * open:        called on open(); rdev = MKDEV(major, minor).  May be NULL
 *              for majors that are listed in /proc/devices but opened through
 *              another filesystem (e.g. devpts).
 *              returns a new open file* on success, NULL on error.
 */
void cdev_register_named(unsigned mode_type, unsigned major,
			 unsigned minor_base, unsigned minor_count,
			 const char *name,
			 file *(*open)(super_block *sb, unsigned rdev,
				       int flag));

#define cdev_register(mode_type, major, minor_base, minor_count, open) \
	cdev_register_named(mode_type, major, minor_base, minor_count, NULL, open)

#define cdev_register_major(mode_type, major, name) \
	cdev_register_named(mode_type, major, 0, 0, name, NULL)

typedef void (*cdev_major_iter_fn)(unsigned mode_type, unsigned major,
				   const char *name, void *data);
void cdev_for_each_major(cdev_major_iter_fn fn, void *data);

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

/*
 * dev_node_add   — create /dev/<name> as a device node (block or char).
 * dev_node_remove — remove /dev/<name>.
 *
 * Can be called after devfs is initialised (KERNEL_INIT >= 6).
 * Used by drivers that hotplug devices (e.g. loop devices).
 */
void dev_node_add(const char *name, unsigned mode, unsigned devno);
void dev_node_remove(const char *name);

#endif /* _DEV_DEV_H */
