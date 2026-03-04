#ifndef _MOUNT_H
#define _MOUNT_H

#include <rbtree.h>
#include <fs.h>
#include <lock.h>

typedef struct super_block super_block;
typedef struct super_operations super_operations;

/*
 * super_operations - filesystem-level callbacks, analogous to Linux
 * struct super_operations.  Each mounted filesystem registers one.
 */
struct super_operations {
	/*
	 * get_root: allocate and return the root inode for pseudo-filesystem
	 * mounts (devices, proc entries).  Called when the mount root or a
	 * sub-path with no specific match is opened.  The returned inode is
	 * owned by the caller and freed via i_fop->release.
	 */
	inode *(*get_root)(super_block *sb);

	/*
	 * open: look up and open a file by path within this filesystem.
	 * Used by real filesystems (e.g. ext4) that own path traversal
	 * internally.  path is relative to this super_block's mount root.
	 * Returns an open file * on success, NULL on failure.
	 * Mutually exclusive with get_root: use one or the other.
	 */
	file *(*open)(super_block *sb, const char *path, int flag);

	/*
	 * put_super: release any private resources held by the filesystem.
	 * Called when the super_block's reference count drops to zero,
	 * just before the super_block itself is freed.
	 */
	void (*put_super)(super_block *sb);
};

/*
 * super_block - in-memory descriptor of a mounted filesystem, analogous
 * to Linux struct super_block.  The kernel keeps one super_block per
 * mount point; child mounts hang off s_mounts.
 */
struct super_block {
	const super_operations *s_op; /* filesystem operations */
	void *s_fs_info; /* private filesystem data */
	unsigned s_ref; /* reference count */
	mutex_t s_lock;
	hash_table *s_mounts; /* child mounts: path → super_block */
	hash_table *s_files; /* pseudo-files: path → debug_inode */
};

/* Allocate and initialise a new super_block with the given operations. */
super_block *sget(const super_operations *s_op);

/* Increment the super_block reference count. */
void sb_get(super_block *sb);

/*
 * Decrement the reference count.  When it reaches zero, s_op->put_super
 * is called (if provided) and the super_block is freed.
 */
void sb_put(super_block *sb);

/* Mount a filesystem with operations s_op at path under sb. */
int vfs_mount(super_block *sb, const char *path, const super_operations *s_op);

/* Unmount the filesystem mounted at path under sb. */
int vfs_umount(super_block *sb, const char *path);

/*
 * Open the file at path, searching from sb downwards.
 * Returns an open file * on success, NULL on failure.
 */
file *vfs_open(super_block *sb, const char *path, int flag);

/*
 * Register a pseudo-file at path under sb.  The fill callback is invoked
 * each time the file is opened to populate its read-only contents.
 */
int vfs_create_file(super_block *sb, const char *path,
		    void (*fill)(void *buf, size_t size));

#endif
