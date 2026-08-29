#include <mm/mm.h>
#include <fs/vfs.h>
#include <fs/fs.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <lib/lock.h>
#include <macro.h>
#include <errno.h>

/**
 * To make sure sorted by path
 */
static int sb_path_comp(const char *left, const char *right)
{
	return 0 - strcmp(left, right);
}

static vfs_mount_node *sb_mount_find(super_block *sb, const char *path)
{
	struct rb_node *node = sb->s_mounts.rb_node;
	while (node) {
		vfs_mount_node *mount = rb_entry(node, vfs_mount_node, rb_node);
		int comp = sb_path_comp(path, mount->path);
		if (comp < 0)
			node = node->rb_left;
		else if (comp > 0)
			node = node->rb_right;
		else
			return mount;
	}
	return NULL;
}

static void sb_mount_insert(super_block *sb, vfs_mount_node *mount)
{
	struct rb_node **link = &sb->s_mounts.rb_node, *parent = NULL;
	while (*link) {
		vfs_mount_node *cur = rb_entry(*link, vfs_mount_node, rb_node);
		parent = *link;
		if (sb_path_comp(mount->path, cur->path) < 0)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&mount->rb_node, parent, link);
	rb_insert_color(&mount->rb_node, &sb->s_mounts);
}

static void sb_mount_remove(super_block *sb, vfs_mount_node *mount)
{
	rb_erase(&mount->rb_node, &sb->s_mounts);
	free(mount->path);
	sb_put(mount->sb);
	free(mount);
}

/*
 * sb_path_resolve - walk the mount tree from sb following path.
 *
 * Descends through child mounts whose keys are strict prefixes of path
 * (prefix must be followed by '/' or '\0' to avoid false matches),
 * returning the deepest super_block that owns path and the remaining
 * path suffix relative to that super_block.
 *
 * Returns 1 on success, 0 if sb or path are NULL.
 */
static int sb_path_resolve(super_block *sb, const char *path,
			   super_block **out_sb, char **out_path)
{
	struct rb_node *node;
	super_block *child;
	const char *rest;
	size_t klen;

	if (!sb || !path)
		return 0;

	if (*path == '\0')
		goto done;

	mutex_lock(&sb->s_lock);

	for (node = rb_first(&sb->s_mounts); node; node = rb_next(node)) {
		vfs_mount_node *mount = rb_entry(node, vfs_mount_node, rb_node);
		klen = strlen(mount->path);
		if (strncmp(path, mount->path, klen) == 0 &&
		    (path[klen] == '/' || path[klen] == '\0')) {
			child = mount->sb;
			rest = path + klen;
			mutex_unlock(&sb->s_lock);
			return sb_path_resolve(child, rest, out_sb, out_path);
		}
	}

	mutex_unlock(&sb->s_lock);

done:
	*out_sb = sb;
	*out_path = (char *)path;
	return 1;
}

super_block *sget(const super_operations *s_op)
{
	super_block *sb = zalloc(sizeof(*sb));
	mutex_init(&sb->s_lock);
	sb->s_mounts = _RBTREE_ROOT_INIT;
	sb->s_op = s_op;
	sb->s_ref = 1;
	return sb;
}

void sb_get(super_block *sb)
{
	__sync_add_and_fetch(&sb->s_ref, 1);
}

void sb_put(super_block *sb)
{
	if (__sync_add_and_fetch(&sb->s_ref, -1) != 0)
		return;

	mutex_lock(&sb->s_lock);

	while (sb->s_mounts.rb_node) {
		struct rb_node *node = rb_first(&sb->s_mounts);
		sb_mount_remove(sb, rb_entry(node, vfs_mount_node, rb_node));
	}

	mutex_unlock(&sb->s_lock);

	if (sb->s_op && sb->s_op->release)
		sb->s_op->release(sb);
	else
		kfree(sb);
}

int vfs_mount(super_block *sb, const char *path, super_block *next)
{
	struct rb_node *node;
	super_block *child;
	char *key;
	size_t klen;

	if (!sb || !path || !next)
		return -EINVAL;

	if (*path != '/')
		return -EINVAL;

	mutex_lock(&sb->s_lock);

	/* Reject duplicate direct registration */
	if (sb_mount_find(sb, path)) {
		mutex_unlock(&sb->s_lock);
		return -EEXIST;
	}

	/* Delegate to an existing child that is a strict prefix of path */
	for (node = rb_first(&sb->s_mounts); node; node = rb_next(node)) {
		vfs_mount_node *mount = rb_entry(node, vfs_mount_node, rb_node);
		klen = strlen(mount->path);
		if (strncmp(path, mount->path, klen) == 0 &&
		    (path[klen] == '/' || path[klen] == '\0')) {
			child = mount->sb;
			mutex_unlock(&sb->s_lock);
			return vfs_mount(child, path + klen, next);
		}
	}

	/* No prefix match: register as a direct child */
	child = next;

	/* Compute child's absolute mountpoint from parent's mountpoint + path.
	 * Parent mountpoint "/" is a special case: child mountpoint = path. */
	if (!sb->s_mountpoint[0] || strcmp(sb->s_mountpoint, "/") == 0) {
		strncpy(child->s_mountpoint, path,
			sizeof(child->s_mountpoint) - 1);
	} else {
		sprintf(child->s_mountpoint, "%s%s", sb->s_mountpoint, path);
	}

	key = strdup(path);
	{
		vfs_mount_node *mount = kmalloc(sizeof(*mount));
		mount->path = key;
		mount->sb = child;
		rb_init_node(&mount->rb_node);
		sb_mount_insert(sb, mount);
	}
	mutex_unlock(&sb->s_lock);
	return 0;
}

void vfs_mount_walk(super_block *sb, void (*cb)(const super_block *, void *),
		    void *arg)
{
	struct rb_node *node;

	if (!sb)
		return;

	/* Emit this superblock if it is a real/pseudo-fs mount. */
	if (sb->s_fstype[0])
		cb(sb, arg);

	/* Recurse into children — release lock while calling back to avoid
	 * deadlock; single-CPU so no structural changes will happen. */
	mutex_lock(&sb->s_lock);
	for (node = rb_first(&sb->s_mounts); node; node = rb_next(node)) {
		vfs_mount_node *mount = rb_entry(node, vfs_mount_node, rb_node);
		super_block *child = mount->sb;
		mutex_unlock(&sb->s_lock);
		vfs_mount_walk(child, cb, arg);
		mutex_lock(&sb->s_lock);
	}
	mutex_unlock(&sb->s_lock);
}

int vfs_umount(super_block *sb, const char *path)
{
	vfs_mount_node *mount;
	struct rb_node *node;
	super_block *child;
	size_t klen;

	if (!sb || !path || *path != '/')
		return -EINVAL;

	mutex_lock(&sb->s_lock);

	/* Direct child mount? */
	mount = sb_mount_find(sb, path);
	if (mount) {
		child = mount->sb;
		sb_mount_remove(sb, mount);
		mutex_unlock(&sb->s_lock);
		return 0;
	}

	/* Prefix match: delegate to child */
	for (node = rb_first(&sb->s_mounts); node; node = rb_next(node)) {
		mount = rb_entry(node, vfs_mount_node, rb_node);
		klen = strlen(mount->path);
		if (strncmp(path, mount->path, klen) == 0 &&
		    (path[klen] == '/' || path[klen] == '\0')) {
			child = mount->sb;
			mutex_unlock(&sb->s_lock);
			return vfs_umount(child, path + klen);
		}
	}

	mutex_unlock(&sb->s_lock);
	return -ENOENT;
}

/*
 * VFS_PATH_OP - resolve a single path through the mount tree and dispatch
 * to the matching super_block's operation.
 * @fn:        the vfs_* function name (used for the recursive descent call)
 * @sop_field: field name in super_operations to invoke
 * @...:       extra arguments forwarded after (sb, path)
 */
#define VFS_PATH_OP(fn, sop_field, ...)                                 \
	do {                                                            \
		super_block *_tsb;                                      \
		char *_rp;                                              \
		if (!sb || !path)                                       \
			return -EINVAL;                                 \
		if (!sb_path_resolve(sb, path, &_tsb, &_rp))            \
			return -EINVAL;                                 \
		if (_tsb != sb)                                         \
			return fn(_tsb, _rp, ##__VA_ARGS__);            \
		if (!_tsb->s_op || !_tsb->s_op->sop_field)              \
			return -ENOSYS;                                 \
		return _tsb->s_op->sop_field(_tsb, _rp, ##__VA_ARGS__); \
	} while (0)

/*
 * VFS_PATH2_OP - resolve two paths through the mount tree, require them to
 * land on the same super_block (-EXDEV otherwise), and dispatch.
 * @sop_field: field name in super_operations to invoke
 */
#define VFS_PATH2_OP(sop_field)                                  \
	do {                                                     \
		super_block *_osb, *_nsb;                        \
		char *_orp, *_nrp;                               \
		if (!sb || !oldpath || !newpath)                 \
			return -EINVAL;                          \
		if (!sb_path_resolve(sb, oldpath, &_osb, &_orp)) \
			return -EINVAL;                          \
		if (!sb_path_resolve(sb, newpath, &_nsb, &_nrp)) \
			return -EINVAL;                          \
		if (_osb != _nsb)                                \
			return -EXDEV;                           \
		if (!_osb->s_op || !_osb->s_op->sop_field)       \
			return -ENOSYS;                          \
		return _osb->s_op->sop_field(_osb, _orp, _nrp);  \
	} while (0)

int vfs_mkdir(super_block *sb, const char *path, unsigned mode)
{
	VFS_PATH_OP(vfs_mkdir, mkdir, mode);
}

int vfs_rmdir(super_block *sb, const char *path)
{
	VFS_PATH_OP(vfs_rmdir, rmdir);
}

int vfs_unlink(super_block *sb, const char *path)
{
	VFS_PATH_OP(vfs_unlink, unlink);
}

int vfs_link(super_block *sb, const char *oldpath, const char *newpath)
{
	VFS_PATH2_OP(link);
}

int vfs_rename(super_block *sb, const char *oldpath, const char *newpath)
{
	VFS_PATH2_OP(rename);
}

/*
 * vfs_symlink: only linkpath is resolved through the mount tree;
 * target is stored verbatim (caller must not pre-resolve it).
 */
int vfs_symlink(super_block *sb, const char *target, const char *linkpath)
{
	super_block *target_sb;
	char *rel_path;

	if (!sb || !target || !linkpath)
		return -EINVAL;
	if (!sb_path_resolve(sb, linkpath, &target_sb, &rel_path))
		return -EINVAL;
	if (target_sb != sb)
		return vfs_symlink(target_sb, target, rel_path);
	if (!target_sb->s_op || !target_sb->s_op->symlink)
		return -ENOSYS;
	return target_sb->s_op->symlink(target_sb, target, rel_path);
}

int vfs_readlink(super_block *sb, const char *path, char *buf, size_t bufsiz,
		 size_t *rcnt)
{
	super_block *target_sb;
	char *rel_path;

	if (!sb || !path || !buf || !bufsiz)
		return -EINVAL;
	if (!sb_path_resolve(sb, path, &target_sb, &rel_path))
		return -EINVAL;
	if (target_sb != sb)
		return vfs_readlink(target_sb, rel_path, buf, bufsiz, rcnt);
	if (!target_sb->s_op || !target_sb->s_op->readlink)
		return -ENOSYS;
	return target_sb->s_op->readlink(target_sb, rel_path, buf, bufsiz,
					 rcnt);
}

/* Defined in src/dev/devnode.c */
super_block *devnode_create(unsigned mode, unsigned rdev);

int vfs_mknod(super_block *sb, const char *path, unsigned mode, unsigned dev)
{
	super_block *node_sb;
	int ret;

	if (!sb || !path || !*path)
		return -EINVAL;

	node_sb = devnode_create(mode, dev);
	if (!node_sb)
		return -ENOMEM;

	ret = vfs_mount(sb, path, node_sb);
	if (ret != 0)
		sb_put(node_sb);

	return ret;
}

int vfs_rmnod(super_block *sb, const char *path)
{
	return vfs_umount(sb, path);
}

int vfs_statfs(super_block *sb, const char *path, struct statfs *buf)
{
	super_block *target_sb;
	char *rel_path;

	if (!sb || !path || !buf)
		return -EINVAL;
	if (!sb_path_resolve(sb, path, &target_sb, &rel_path))
		return -EINVAL;
	if (!target_sb->s_op || !target_sb->s_op->statfs)
		return -ENOSYS;
	return target_sb->s_op->statfs(target_sb, buf);
}

int vfs_utime(super_block *sb, const char *path, unsigned atime, unsigned mtime)
{
	VFS_PATH_OP(vfs_utime, utime, atime, mtime);
}

file *vfs_open(super_block *sb, const char *path, int flag)
{
	super_block *target_sb;
	char *rel_path;

	if (!sb || !path)
		return NULL;

	if (!sb_path_resolve(sb, path, &target_sb, &rel_path))
		return NULL;

	/* sb_path_resolve descends into children; if target differs, re-enter */
	if (target_sb != sb)
		return vfs_open(target_sb, rel_path, flag);

	/* Opening the mount root: empty suffix or bare trailing slash */
	if (*rel_path == '\0' || (rel_path[0] == '/' && rel_path[1] == '\0')) {
		if (!target_sb->s_op || !target_sb->s_op->open_root)
			return NULL;
		return target_sb->s_op->open_root(target_sb, flag);
	}

	/*
	 * Real filesystem (e.g. ext4): delegate full path resolution to the
	 * filesystem's own open operation.
	 */
	if (target_sb->s_op && target_sb->s_op->open)
		return target_sb->s_op->open(target_sb, rel_path, flag);

	/*
	 * Pseudo-filesystem fallback: the super_block has a root inode but
	 * no path-aware open (e.g. a block device mount accessed via a
	 * trailing slash).
	 */
	if (target_sb->s_op && target_sb->s_op->open_root) {
		return target_sb->s_op->open_root(target_sb, flag);
	}

	return NULL;
}
