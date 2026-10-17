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
	 * open_root: open the exact mount root, with an optional trailing slash.
	 * Descendant lookup requires the open operation. The returned file is
	 * owned by the caller and released through fs_put_file().
	 */
	file *(*open_root)(super_block *sb, int flag);

	/*
	 * open: look up and open a file by path within this filesystem.
	 * Used by real filesystems (e.g. ext4) that own path traversal
	 * internally.  path is relative to this super_block's mount root.
	 * Returns an open file * on success, NULL on failure.
	 * Mount-root and descendant operations may coexist.
	 */
	file *(*open)(super_block *sb, const char *path, int flag);

	/* Optional lookup for followed final symlinks. Return an open file, or
	 * NULL with *target set to an absolute target pathname owned by the
	 * caller (release with name_put). NULL with no target means failure.
	 * Explicit O_NOFOLLOW opens use open instead, preserving link objects. */
	file *(*open_link)(super_block *sb, const char *path, int flag,
			   char **target);

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
	int (*statfs)(super_block *sb, struct statfs64 *buf);
	int (*utime)(super_block *sb, const char *path, unsigned atime,
		     unsigned mtime);
	/* remount: change mount flags on an already-mounted filesystem.
	 * Called by fs_do_mount() when MS_REMOUNT is set. */
	int (*remount)(super_block *sb, int flags);
};

/*
 * super_block - in-memory descriptor of a mounted filesystem, analogous
 * to Linux struct super_block.  The kernel keeps one super_block per
 * mount point; child mounts hang off s_mounts.
 */
struct super_block {
	const super_operations *s_op; /* filesystem operations */
	void *s_fs_info; /* private filesystem data */
	super_block *
		s_covered; /* Covered directory or mount, owned by this layer. */
	super_block *s_backend; /* Retained owner of a private mount view. */
	unsigned s_ref; /* reference count */
	unsigned s_mount_id; /* unique mount identity */
	unsigned s_peer_group, s_master_group, s_unbindable;
	int s_flags; /* mount flags (MS_RDONLY etc.) */
	char s_devname[64]; /* source device, e.g. "hda1" or "proc" */
	char s_fstype[32]; /* filesystem type, e.g. "ext4"; empty = not a real mount */
	char s_mountpoint[256]; /* absolute mountpoint, set by vfs_mount() */
	mutex_t s_lock;
	struct rb_root s_mounts; /* child mounts, ordered by path */
	/* Direct-child lookup results, protected by s_lock and cleared on edits. */
	struct {
		char path[256];
		struct _vfs_mount_node *mount;
		size_t length;
		int valid;
	} s_lookup[4];
	unsigned s_lookup_last;
};

typedef struct _vfs_mount_node {
	char *path;
	super_block *sb;
	struct rb_node rb_node;
} vfs_mount_node;

/* Allocate and initialise a new super_block with the given operations. */
super_block *sget(const super_operations *s_op);
super_block *vfs_clone_mount_tree(super_block *sb);
super_block *vfs_clone_bind_tree(super_block *sb);
super_block *vfs_bind_mount(super_block *root, const char *path, int recursive);

/* Increment the super_block reference count. */
void sb_get(super_block *sb);

/*
 * Decrement the reference count.  When it reaches zero, s_op->release
 * is called (if provided) and the super_block is freed.
 */
void sb_put(super_block *sb);

/* Resolve filesystem identity and its path suffix without acquiring a reference. */
int sb_path_resolve(super_block *sb, const char *path, super_block **out_sb,
		    char **out_path);

/* Invalidate followed-link paths around namespace changes. */
void vfs_mount_changed(void);
uint64_t vfs_mount_generation(void);
unsigned vfs_mount_poll(uint64_t seen, unsigned events, poll_table *pt);
void vfs_namespace_track(void *token, super_block *root);
void vfs_namespace_untrack(void *token);
int vfs_set_propagation(super_block *root, const char *target, unsigned flags);
int vfs_mount_propagated(super_block *root, const char *target, super_block *mount);
int vfs_umount_propagated(super_block *root, const char *target);
void vfs_namespace_changed(void);
uint64_t vfs_namespace_generation(void);

/* Mount a filesystem with operations child at path under sb. */
int vfs_mount(super_block *sb, const char *path, super_block *child);

/* Unmount the filesystem mounted at path under sb. */
int vfs_umount(super_block *sb, const char *path);

/*
 * Open the file at path, searching from sb downwards.
 * Returns an open file * on success, NULL on failure.
 */
file *vfs_open(super_block *sb, const char *path, int flag);

/* Record backend provenance once; delegated lookups preserve the inner owner. */
void vfs_set_file_origin(file *fp, super_block *sb, const char *relative_path);

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

/*
 * vfs_rmmode - remove a special file (device node)
 */
int vfs_rmnod(super_block *sb, const char *path);

/* Fill buf with filesystem statistics for the filesystem owning path. */
int vfs_statfs(super_block *sb, const char *path, struct statfs64 *buf);
int vfs_utime(super_block *sb, const char *path, unsigned atime,
	      unsigned mtime);

/*
 * vfs_mount_walk - walk every mounted super_block in DFS order, calling cb
 * for each one that has s_fstype set (i.e. a real/pseudo filesystem mount,
 * not an internal devnode or proc-entry super_block).
 * Used by /proc/mounts.
 */
void vfs_mount_walk(super_block *sb, void (*cb)(const super_block *, void *),
		    void *arg);

super_block *vfs_take_mount(super_block *sb, const char *path);
void vfs_rebase_mount_tree(super_block *sb, const char *point);
#endif
