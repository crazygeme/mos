#ifndef _FS_MOUNT_H
#define _FS_MOUNT_H

#include <lib/rbtree.h>
#include <lib/lock.h>
#include <fs/fs.h>
#include <unistd.h>

typedef struct super_block super_block;
typedef struct super_operations super_operations;

/*
 * super_operations - filesystem-level callbacks, analogous to Linux
 * struct super_operations.  Each mounted filesystem registers one.
 */
struct super_operations {
	/*
	 * get_root: allocate and return the root inode for
	 * mounts (devices, proc entries).  Called when the mount root or a
	 * sub-path with no specific match is opened.  The returned inode is
	 * owned by the caller and freed via i_fop->release.
	 */
	file *(*open_root)(super_block *sb, int flag);

	/*
	 * open: look up and open a file by path within this filesystem.
	 * Used by real filesystems (e.g. ext4) that own path traversal
	 * internally.  path is relative to this super_block's mount root.
	 * Returns an open file * on success, NULL on failure.
	 * Mutually exclusive with get_root: use one or the other.
	 */
	file *(*open)(super_block *sb, const char *path, int flag);

	/*
	 * put_super: Custom dtor of super_block.
	 * Called when the super_block's reference count drops to zero.
	 * If zero just system will just call regular kfree.
	 */
	void (*release)(super_block *sb);

	/* Directory and link operations.  path/oldpath/newpath are relative
	 * to this super_block's mount root.  Return 0 on success, -errno. */
	int (*mkdir)(super_block *sb, const char *path, unsigned mode);
	int (*rmdir)(super_block *sb, const char *path);
	int (*unlink)(super_block *sb, const char *path);
	/* link: create a hard link from oldpath to newpath (both on same sb) */
	int (*link)(super_block *sb, const char *oldpath, const char *newpath);
	/* symlink: create a symlink at linkpath whose content is target */
	int (*symlink)(super_block *sb, const char *target,
		       const char *linkpath);
	int (*rename)(super_block *sb, const char *oldpath,
		      const char *newpath);
	int (*readlink)(super_block *sb, const char *path, char *buf,
			size_t bufsiz, size_t *rcnt);
	int (*statfs)(super_block *sb, struct statfs *buf);
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
};

/* Allocate and initialise a new super_block with the given operations. */
super_block *sget(const super_operations *s_op);

/* Increment the super_block reference count. */
void sb_get(super_block *sb);

/*
 * Decrement the reference count.  When it reaches zero, s_op->release
 * is called (if provided) and the super_block is freed.
 */
void sb_put(super_block *sb);

/* Mount a filesystem with operations child at path under sb. */
int vfs_mount(super_block *sb, const char *path, super_block *child);

/* Unmount the filesystem mounted at path under sb. */
int vfs_umount(super_block *sb, const char *path);

/*
 * Open the file at path, searching from sb downwards.
 * Returns an open file * on success, NULL on failure.
 */
file *vfs_open(super_block *sb, const char *path, int flag);

/* VFS-level directory and link operations.  All paths are absolute.
 * Return 0 on success, -errno on failure.
 * vfs_link and vfs_rename require both paths to be on the same mount (-EXDEV
 * otherwise).  vfs_symlink stores target verbatim (not resolved). */
int vfs_mkdir(super_block *sb, const char *path, unsigned mode);
int vfs_rmdir(super_block *sb, const char *path);
int vfs_unlink(super_block *sb, const char *path);
int vfs_link(super_block *sb, const char *oldpath, const char *newpath);
int vfs_symlink(super_block *sb, const char *target, const char *linkpath);
int vfs_rename(super_block *sb, const char *oldpath, const char *newpath);
int vfs_readlink(super_block *sb, const char *path, char *buf, size_t bufsiz,
		 size_t *rcnt);

/*
 * vfs_mknod - create a special file (device node, named FIFO, socket) at
 * path by mounting an in-memory devnode super_block there.
 * mode: full mode bits (e.g. S_IFCHR | 0666).
 * dev:  encoded major/minor (ignored for S_IFIFO / S_IFSOCK).
 */
int vfs_mknod(super_block *sb, const char *path, unsigned mode, unsigned dev);

/* Fill buf with filesystem statistics for the filesystem owning path. */
int vfs_statfs(super_block *sb, const char *path, struct statfs *buf);

#endif
