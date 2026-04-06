#ifndef _FS_MOUNT_SYSCALL_H
#define _FS_MOUNT_SYSCALL_H

#include <fs/vfs.h>

/* Standard mount flags (subset of Linux MS_* values) */
#define MS_RDONLY 1
#define MS_REMOUNT 32

/*
 * fs_type - describes a mountable filesystem.  Each filesystem registers
 * itself via fs_register_type() so that sys_mount() can look it up by name.
 */
typedef struct fs_type fs_type;
struct fs_type {
	const char *name;
	/*
	 * get_sb: allocate and return a new super_block for this filesystem.
	 * @dev:    source device / image path (may be NULL for pseudo-fs)
	 * @target: absolute mount point path (e.g. "/mnt")
	 * @flags:  mount flags (MS_RDONLY etc.)
	 * @data:   filesystem-specific mount options (may be NULL)
	 * Returns NULL on failure.
	 */
	super_block *(*get_sb)(const char *dev, const char *target, int flags,
			       void *data);
	struct fs_type
		*next; /* intrusive linked list — set by fs_register_type */
};

/* Add a filesystem type to the global registry. */
void fs_register_type(fs_type *fst);

/*
 * fs_do_mount - core of sys_mount().
 * Looks up @type in the registry, instantiates a super_block, and calls
 * vfs_mount().  If the path is already mounted (-EEXIST), returns 0.
 */
int fs_do_mount(const char *dev, const char *target, const char *type,
		unsigned flags, void *data);

/*
 * fs_do_umount - core of sys_umount().
 * Calls vfs_umount() on the current task's root.
 */
int fs_do_umount(const char *target, int flags);

/*
 * fs_sync_super - flush filesystem-managed dirty state for one mounted
 * super_block. Filesystems without explicit sync support may return 0.
 */
int fs_sync_super(const super_block *sb);

#endif
