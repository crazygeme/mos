#include <mm/mm.h>
#include <fs/vfs.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/inotify.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <lib/lock.h>
#include <macro.h>
#include <errno.h>
#include <ps/ps.h>

/**
 * To make sure sorted by path
 */
static int sb_path_comp(const char *left, const char *right)
{
	return 0 - strcmp(left, right);
}

static vfs_mount_node *sb_mount_find_n(super_block *sb, const char *path,
				       size_t length)
{
	struct rb_node *node = sb->s_mounts.rb_node;
	while (node) {
		vfs_mount_node *mount = rb_entry(node, vfs_mount_node, rb_node);
		int comp = strncmp(mount->path, path, length);
		if (!comp && mount->path[length])
			comp = 1;
		if (!comp)
			return mount;
		node = comp < 0 ? node->rb_left : node->rb_right;
	}
	return NULL;
}

static vfs_mount_node *sb_mount_find(super_block *sb, const char *path)
{
	return sb_mount_find_n(sb, path, strlen(path));
}

/* Search complete path components, from the deepest mount toward the root. */
static vfs_mount_node *sb_mount_prefix(super_block *sb, const char *path,
				       size_t *length)
{
	size_t end = strlen(path);
	while (end) {
		vfs_mount_node *mount = sb_mount_find_n(sb, path, end);
		if (mount) {
			*length = end;
			return mount;
		}
		while (end && path[--end] != '/')
			;
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
int sb_path_resolve(super_block *sb, const char *path, super_block **out_sb,
		    char **out_path)
{
	size_t length;
	vfs_mount_node *mount;

	if (!sb || !path)
		return 0;

	if (!*path)
		goto done;
	mutex_lock(&sb->s_lock);
	mount = sb_mount_prefix(sb, path, &length);
	if (mount) {
		super_block *child = mount->sb;
		mutex_unlock(&sb->s_lock);
		return sb_path_resolve(child, path + length, out_sb, out_path);
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
	if (!sb)
		return NULL;
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
	vfs_mount_node *prefix;
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

	/* Delegate through the indexed child mount. */
	prefix = sb_mount_prefix(sb, path, &klen);
	if (prefix) {
		child = prefix->sb;
		mutex_unlock(&sb->s_lock);
		return vfs_mount(child, path + klen, next);
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

	vfs_mount_node *mount = kmalloc(sizeof(*mount));
	mount->path = key;
	mount->sb = child;
	rb_init_node(&mount->rb_node);
	sb_mount_insert(sb, mount);

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
	super_block *child;
	size_t klen;
	inotify_node *removed;

	if (!sb || !path || *path != '/')
		return -EINVAL;
	removed = inotify_snapshot(sb, path);

	mutex_lock(&sb->s_lock);

	/* Direct child mount? */
	mount = sb_mount_find(sb, path);
	if (mount) {
		child = mount->sb;
		sb_get(child);
		sb_mount_remove(sb, mount);
		mutex_unlock(&sb->s_lock);
		if (!child->s_fstype[0])
			inotify_removed(removed);
		inotify_unmounted(child);
		inotify_snapshot_put(removed);
		sb_put(child);
		return 0;
	}

	/* Delegate through the indexed child mount. */
	mount = sb_mount_prefix(sb, path, &klen);
	if (mount) {
		child = mount->sb;
		sb_get(child);
		mutex_unlock(&sb->s_lock);
		inotify_snapshot_put(removed);
		int ret = vfs_umount(child, path + klen);
		sb_put(child);
		return ret;
	}

	mutex_unlock(&sb->s_lock);
	inotify_snapshot_put(removed);
	return -ENOENT;
}

/*
 * VFS_PATH_RESULT - resolve a single path through the mount tree and dispatch
 * to the matching super_block's operation.
 * @sop_field: field name in super_operations to invoke
 * @...:       extra arguments forwarded after (sb, path)
 */
#define VFS_PATH_RESULT(result, sop_field, ...)                              \
	do {                                                                 \
		super_block *_tsb;                                           \
		char *_rp;                                                   \
		if (!sb || !path || !sb_path_resolve(sb, path, &_tsb, &_rp)) \
			(result) = -EINVAL;                                  \
		else if (!_tsb->s_op || !_tsb->s_op->sop_field)              \
			(result) = -ENOSYS;                                  \
		else                                                         \
			(result) = _tsb->s_op->sop_field(_tsb, _rp,          \
							 ##__VA_ARGS__);     \
	} while (0)

/*
 * VFS_PATH2_RESULT - resolve two paths through the mount tree, require them to
 * land on the same super_block (-EXDEV otherwise), and dispatch.
 * @sop_field: field name in super_operations to invoke
 */
#define VFS_PATH2_RESULT(result, sop_field)                                 \
	do {                                                                \
		super_block *_osb, *_nsb;                                   \
		char *_orp, *_nrp;                                          \
		if (!sb || !oldpath || !newpath ||                          \
		    !sb_path_resolve(sb, oldpath, &_osb, &_orp) ||          \
		    !sb_path_resolve(sb, newpath, &_nsb, &_nrp))            \
			(result) = -EINVAL;                                 \
		else if (_osb != _nsb)                                      \
			(result) = -EXDEV;                                  \
		else if (!_osb->s_op || !_osb->s_op->sop_field)             \
			(result) = -ENOSYS;                                 \
		else                                                        \
			(result) = _osb->s_op->sop_field(_osb, _orp, _nrp); \
	} while (0)

int vfs_mkdir(super_block *sb, const char *path, unsigned mode)
{
	int ret;
	VFS_PATH_RESULT(ret, mkdir, mode);
	if (!ret)
		inotify_created(sb, path);
	return ret;
}

int vfs_rmdir(super_block *sb, const char *path)
{
	inotify_node *node = inotify_snapshot(sb, path);
	int ret;
	VFS_PATH_RESULT(ret, rmdir);
	if (!ret)
		inotify_removed(node);
	inotify_snapshot_put(node);
	return ret;
}

int vfs_unlink(super_block *sb, const char *path)
{
	inotify_node *node = inotify_snapshot(sb, path);
	int ret;
	VFS_PATH_RESULT(ret, unlink);
	if (!ret)
		inotify_removed(node);
	inotify_snapshot_put(node);
	return ret;
}

int vfs_link(super_block *sb, const char *oldpath, const char *newpath)
{
	inotify_node *node = inotify_snapshot(sb, oldpath);
	int ret;
	VFS_PATH2_RESULT(ret, link);
	if (!ret)
		inotify_linked(node, sb, newpath);
	inotify_snapshot_put(node);
	return ret;
}

int vfs_rename(super_block *sb, const char *oldpath, const char *newpath)
{
	inotify_node *source = inotify_snapshot(sb, oldpath);
	inotify_node *replacement = inotify_snapshot(sb, newpath);
	int ret;
	VFS_PATH2_RESULT(ret, rename);
	if (!ret)
		inotify_renamed(source, replacement, sb, newpath);
	inotify_snapshot_put(source);
	inotify_snapshot_put(replacement);
	return ret;
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
	if (!target_sb->s_op || !target_sb->s_op->symlink)
		return -ENOSYS;
	int ret = target_sb->s_op->symlink(target_sb, target, rel_path);
	if (!ret)
		inotify_created(sb, linkpath);
	return ret;
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
	if (!target_sb->s_op || !target_sb->s_op->readlink) {
		file *fp;

		/* A filesystem without symlinks still supports readlink errors
		 * for existing non-links and missing paths. */
		if (*rel_path && strcmp(rel_path, "/") != 0 &&
		    (!target_sb->s_op || !target_sb->s_op->open))
			return -ENOENT;
		fp = vfs_open(target_sb, rel_path, O_PATH | O_NOFOLLOW);
		if (!fp)
			return -ENOENT;
		fs_put_file(fp);
		return -EINVAL;
	}
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
	else
		inotify_created(sb, path);

	return ret;
}

int vfs_rmnod(super_block *sb, const char *path)
{
	return vfs_umount(sb, path);
}

int vfs_statfs(super_block *sb, const char *path, struct statfs64 *buf)
{
	super_block *target_sb;
	char *rel_path;
	int ret;

	if (!sb || !path || !buf)
		return -EINVAL;
	if (!sb_path_resolve(sb, path, &target_sb, &rel_path))
		return -EINVAL;
	if (!target_sb->s_op || !target_sb->s_op->statfs)
		return -ENOSYS;
	ret = target_sb->s_op->statfs(target_sb, buf);
	if (!ret)
		buf->f_flags = 0x20 | (target_sb->s_flags &
				       3); /* ST_VALID, RO, NOSUID */
	return ret;
}

int vfs_utime(super_block *sb, const char *path, unsigned atime, unsigned mtime)
{
	int ret;
	VFS_PATH_RESULT(ret, utime, atime, mtime);
	if (!ret)
		inotify_path_event(sb, path, IN_ATTRIB);
	return ret;
}

void vfs_set_file_origin(file *fp, super_block *sb, const char *relative_path)
{
	size_t length;
	if (!fp || fp->f_sb || !sb)
		return;
	fp->f_sb = sb;
	sb_get(sb);
	if (fp->f_inode && fp->f_inode->i_ino)
		return;
	length = strlen(relative_path);
	while (length && relative_path[length - 1] == '/')
		length--;
	fp->f_relative_path = malloc(length + 1);
	if (fp->f_relative_path) {
		memcpy(fp->f_relative_path, relative_path, length);
		fp->f_relative_path[length] = 0;
	}
}

static file *vfs_open_raw(super_block *sb, const char *path, int flag,
			  char **link_target)
{
	super_block *target_sb;
	char *rel_path;
	file *fp;

	if (!sb || !path)
		return NULL;

	if (!sb_path_resolve(sb, path, &target_sb, &rel_path))
		return NULL;

	/* Opening the mount root: empty suffix or bare trailing slash */
	if (*rel_path == '\0' || (rel_path[0] == '/' && rel_path[1] == '\0')) {
		if (!target_sb->s_op || !target_sb->s_op->open_root)
			return NULL;
		fp = target_sb->s_op->open_root(target_sb, flag);
		if (fp) {
			vfs_set_file_origin(fp, target_sb, rel_path);
			fp->f_mount_flags = target_sb->s_flags;
		}
		return fp;
	}

	/*
	 * Real filesystem (e.g. ext4): delegate full path resolution to the
	 * filesystem's own open operation.
	 */
	if (target_sb->s_op && target_sb->s_op->open) {
		if (link_target && target_sb->s_op->open_link)
			fp = target_sb->s_op->open_link(target_sb, rel_path,
							flag, link_target);
		else
			fp = target_sb->s_op->open(target_sb, rel_path, flag);
		if (fp) {
			vfs_set_file_origin(fp, target_sb, rel_path);
			fp->f_mount_flags = target_sb->s_flags;
		}
		return fp;
	}

	return NULL;
}

/* Follow final symlinks through the mount tree, including ext4-to-proc links. */
file *vfs_open(super_block *sb, const char *path, int flag)
{
	file *fp;
	char *target = NULL, *joined = NULL;
	const char *lookup = path;
	super_block *lookup_sb = sb;
	unsigned depth;

	if (!sb || !path)
		return NULL;
	if (flag & O_NOFOLLOW)
		return vfs_open_raw(sb, path, flag, NULL);
	for (depth = 0; depth <= 40; depth++) {
		size_t len = 0;
		const char *linkpath, *slash;
		size_t base;
		int ret;
		char *resolved_target = NULL;

		fp = vfs_open_raw(lookup_sb, lookup, flag | O_NOFOLLOW,
				  &resolved_target);
		if (resolved_target) {
			if (depth == 40) {
				name_put(resolved_target);
				break;
			}
			if (joined)
				name_put(joined);
			joined = resolved_target;
			goto normalize_link;
		}
		if (!fp || !fp->f_inode || !S_ISLNK(fp->f_inode->i_mode))
			goto out;
		if (fp->f_fop && fp->f_fop->follow_link) {
			file *target_file = fp->f_fop->follow_link(fp, flag);
			fs_put_file(fp);
			fp = target_file;
			goto out;
		}
		if (depth == 40) {
			fs_put_file(fp);
			break;
		}
		if (!target)
			target = name_get();
		if (!joined)
			joined = name_get();
		if (!target || !joined) {
			fs_put_file(fp);
			break;
		}
		if (fp->f_fop && fp->f_fop->readlink)
			ret = fp->f_fop->readlink(fp, target, MAX_PATH - 1,
						  &len);
		else
			ret = vfs_readlink(lookup_sb, lookup, target,
					   MAX_PATH - 1, &len);
		if (ret || !len || len >= MAX_PATH) {
			fs_put_file(fp);
			break;
		}
		target[len] = '\0';
		if (target[0] == '/') {
			strcpy(joined, target);
		} else {
			linkpath = fp->f_name ? fp->f_name : lookup;
			slash = strrchr(linkpath, '/');
			base = slash ? (size_t)(slash - linkpath) + 1 : 0;
			if (!base || base + len >= MAX_PATH) {
				fs_put_file(fp);
				break;
			}
			memmove(joined, linkpath, base);
			memcpy(joined + base, target, len + 1);
		}
		fs_put_file(fp);
normalize_link:
		/* Normalize dot components before selecting a mount. */
		{
			const char *src = joined;
			char *dst = joined;
			*dst++ = '/';
			while (*src) {
				const char *start;
				while (*src == '/')
					src++;
				start = src;
				while (*src && *src != '/')
					src++;
				len = src - start;
				if (!len || (len == 1 && start[0] == '.'))
					continue;
				if (len == 2 && start[0] == '.' &&
				    start[1] == '.') {
					while (dst > joined + 1 &&
					       *--dst != '/')
						;
					continue;
				}
				if (dst > joined + 1)
					*dst++ = '/';
				memmove(dst, start, len);
				dst += len;
			}
			*dst = '\0';
		}
		lookup = joined;
		lookup_sb = current->fs->root;
	}
	fp = NULL;
out:
	if (fp && !fp->f_name)
		fp->f_name = strdup(lookup);
	if (joined)
		name_put(joined);
	if (target)
		name_put(target);
	return fp;
}
