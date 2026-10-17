#include <device/devnode.h>
#include <mm/mm.h>
#include <fs/vfs.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
#include <fs/inotify.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <lib/lock.h>
#include <macro.h>
#include <errno.h>
#include <ps/ps.h>

static uint64_t mount_generation = 1;
static spinlock_t mount_event_lock = SPINLOCK_INITIALIZER;
static list_entry mount_waiters = { &mount_waiters, &mount_waiters };
uint64_t vfs_mount_generation(void)
{
#if __SIZEOF_POINTER__ == 4
	return __sync_val_compare_and_swap(&mount_generation, 0, 0);
#else
	return __atomic_load_n(&mount_generation, __ATOMIC_ACQUIRE);
#endif
}
void vfs_mount_changed(void)
{
	int irq;
	spinlock_lock(&mount_event_lock, &irq);
	__sync_add_and_fetch(&mount_generation, 1);
	poll_notify(&mount_waiters);
	spinlock_unlock(&mount_event_lock, irq);
}
unsigned vfs_mount_poll(uint64_t seen, unsigned events, poll_table *pt)
{
	int irq;
	poll_subscribe(pt, &mount_waiters, &mount_event_lock);
	spinlock_lock(&mount_event_lock, &irq);
	unsigned ready = seen != vfs_mount_generation() ?
		FS_POLL_EXCEPT | FS_POLL_ERR : 0;
	spinlock_unlock(&mount_event_lock, irq);
	return (events & FS_POLL_READ) | ready;
}

static uint64_t namespace_generation = 1;

void vfs_namespace_changed(void)
{
	__atomic_add_fetch(&namespace_generation, 1, __ATOMIC_RELEASE);
}

uint64_t vfs_namespace_generation(void)
{
#if __SIZEOF_POINTER__ == 4
	/* GCC implements an i686 atomic 64-bit load with x87 instructions.
	 * The kernel must not touch userspace FPU state during pathname lookup. */
	return __sync_val_compare_and_swap(&namespace_generation, 0, 0);
#else
	return __atomic_load_n(&namespace_generation, __ATOMIC_ACQUIRE);
#endif
}

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
	unsigned last = sb->s_lookup_last;
	if (sb->s_lookup[last].valid &&
	    !strcmp(sb->s_lookup[last].path, path)) {
		*length = sb->s_lookup[last].length;
		return sb->s_lookup[last].mount;
	}
	size_t end = strlen(path);
	unsigned slot = 0;
	size_t path_length = end;
	for (size_t i = 0; i < path_length; i++)
		slot = slot * 33 + (unsigned char)path[i];
	slot %= 4;
	if (path_length < sizeof(sb->s_lookup[slot].path) &&
	    sb->s_lookup[slot].valid &&
	    !strcmp(sb->s_lookup[slot].path, path)) {
		sb->s_lookup_last = slot;
		*length = sb->s_lookup[slot].length;
		return sb->s_lookup[slot].mount;
	}
	vfs_mount_node *found = NULL;
	while (end) {
		vfs_mount_node *mount = sb_mount_find_n(sb, path, end);
		if (mount) {
			*length = end;
			found = mount;
			break;
		}
		while (end && path[--end] != '/')
			;
	}
	if (path_length < sizeof(sb->s_lookup[slot].path)) {
		memcpy(sb->s_lookup[slot].path, path, path_length + 1);
		sb->s_lookup[slot].mount = found;
		sb->s_lookup[slot].length = found ? end : 0;
		sb->s_lookup[slot].valid = 1;
		sb->s_lookup_last = slot;
	}
	return found;
}

static void sb_mount_insert(super_block *sb, vfs_mount_node *mount)
{
	vfs_namespace_changed();
	memset(sb->s_lookup, 0, sizeof(sb->s_lookup));
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
	vfs_namespace_changed();
}

static void sb_mount_remove(super_block *sb, vfs_mount_node *mount)
{
	vfs_namespace_changed();
	memset(sb->s_lookup, 0, sizeof(sb->s_lookup));
	rb_erase(&mount->rb_node, &sb->s_mounts);
	free(mount->path);
	sb_put(mount->sb);
	free(mount);
	vfs_namespace_changed();
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

static unsigned next_mount_id;

super_block *sget(const super_operations *s_op)
{
	super_block *sb = zalloc(sizeof(*sb));
	if (!sb)
		return NULL;
	mutex_init(&sb->s_lock);
	sb->s_mounts = _RBTREE_ROOT_INIT;
	sb->s_op = s_op;
	sb->s_ref = 1;
	sb->s_mount_id = __sync_add_and_fetch(&next_mount_id, 1);
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

	if (sb->s_covered)
		sb_put(sb->s_covered);
	if (sb->s_backend) {
		sb_put(sb->s_backend);
		kfree(sb);
	} else if (sb->s_op && sb->s_op->release)
		sb->s_op->release(sb);
	else
		kfree(sb);
}

static super_block *clone_mount_tree(super_block *old, int prune)
{
	if (!old)
		return NULL;
	super_block *copy = sget(old->s_op);
	if (!copy)
		return NULL;
	copy->s_backend = old->s_backend ? old->s_backend : old;
	sb_get(copy->s_backend);
	copy->s_fs_info = old->s_fs_info;
	if (old->s_covered) {
		copy->s_covered = clone_mount_tree(old->s_covered, prune);
		if (!copy->s_covered) {
			sb_put(copy);
			return NULL;
		}
	}
	copy->s_flags = old->s_flags;
	copy->s_peer_group = old->s_peer_group;
	copy->s_master_group = old->s_master_group;
	copy->s_unbindable = old->s_unbindable;
	strcpy(copy->s_devname, old->s_devname);
	strcpy(copy->s_fstype, old->s_fstype);
	strcpy(copy->s_mountpoint, old->s_mountpoint);
	mutex_lock(&old->s_lock);
	for (struct rb_node *n = rb_first(&old->s_mounts); n; n = rb_next(n)) {
		vfs_mount_node *mount = rb_entry(n, vfs_mount_node, rb_node);
		if (prune && mount->sb->s_unbindable)
			continue;
		super_block *child = clone_mount_tree(mount->sb, prune);
		vfs_mount_node *entry = zalloc(sizeof(*entry));
		if (!child || !entry) {
			if (child)
				sb_put(child);
			free(entry);
			mutex_unlock(&old->s_lock);
			sb_put(copy);
			return NULL;
		}
		entry->path = strdup(mount->path);
		if (!entry->path) {
			sb_put(child);
			free(entry);
			mutex_unlock(&old->s_lock);
			sb_put(copy);
			return NULL;
		}
		entry->sb = child;
		sb_mount_insert(copy, entry);
	}
	mutex_unlock(&old->s_lock);
	return copy;
}

super_block *vfs_clone_mount_tree(super_block *old)
{
	return clone_mount_tree(old, 0);
}
super_block *vfs_clone_bind_tree(super_block *old)
{
	return clone_mount_tree(old, 1);
}

void vfs_rebase_mount_tree(super_block *sb, const char *point)
{
	strncpy(sb->s_mountpoint, point, sizeof(sb->s_mountpoint) - 1);
	mutex_lock(&sb->s_lock);
	for (struct rb_node *n = rb_first(&sb->s_mounts); n; n = rb_next(n)) {
		vfs_mount_node *mount = rb_entry(n, vfs_mount_node, rb_node);
		char path[256];
		if (!strcmp(point, "/"))
			strcpy(path, mount->path);
		else {
			if (strlen(point) + strlen(mount->path) >= sizeof(path))
				continue;
			sprintf(path, "%s%s", point, mount->path);
		}
		vfs_rebase_mount_tree(mount->sb, path);
	}
	mutex_unlock(&sb->s_lock);
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

	vfs_mount_node *existing = sb_mount_find(sb, path);
	if (existing) {
		if (!next->s_fstype[0]) {
			mutex_unlock(&sb->s_lock);
			return -EEXIST;
		}
		file *covered = existing->sb->s_op && existing->sb->s_op->open_root ?
			existing->sb->s_op->open_root(existing->sb, O_PATH) : NULL;
		file *incoming = next->s_op && next->s_op->open_root ?
			next->s_op->open_root(next, O_PATH) : NULL;
		int compatible = covered && incoming && covered->f_inode && incoming->f_inode &&
			!!S_ISDIR(covered->f_inode->i_mode) == !!S_ISDIR(incoming->f_inode->i_mode);
		if (covered) fs_put_file(covered);
		if (incoming) fs_put_file(incoming);
		if (!compatible) {
			mutex_unlock(&sb->s_lock);
			return -ENOTDIR;
		}
		vfs_rebase_mount_tree(next, existing->sb->s_mountpoint);
		next->s_covered = existing->sb;
		existing->sb = next;
		memset(sb->s_lookup, 0, sizeof(sb->s_lookup));
		vfs_namespace_changed();
		mutex_unlock(&sb->s_lock);
		return 0;
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

	char point[256];
	strcpy(point, child->s_mountpoint);
	vfs_rebase_mount_tree(child, point);
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

super_block *vfs_take_mount(super_block *sb, const char *path)
{
	if (!sb || !path || *path != '/')
		return NULL;
	mutex_lock(&sb->s_lock);
	vfs_mount_node *mount = sb_mount_find(sb, path);
	if (mount) {
		super_block *result = mount->sb;
		if (result->s_covered) {
			mount->sb = result->s_covered;
			result->s_covered = NULL;
		} else {
			rb_erase(&mount->rb_node, &sb->s_mounts);
			free(mount->path);
			free(mount);
		}
		memset(sb->s_lookup, 0, sizeof(sb->s_lookup));
		vfs_namespace_changed();
		mutex_unlock(&sb->s_lock);
		return result;
	}
	size_t length;
	mount = sb_mount_prefix(sb, path, &length);
	super_block *child = mount ? mount->sb : NULL;
	if (child)
		sb_get(child);
	mutex_unlock(&sb->s_lock);
	if (!child)
		return NULL;
	super_block *result = vfs_take_mount(child, path + length);
	sb_put(child);
	return result;
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
		if (child->s_covered) {
			mount->sb = child->s_covered;
			child->s_covered = NULL;
			sb_put(child);
			memset(sb->s_lookup, 0, sizeof(sb->s_lookup));
			vfs_namespace_changed();
		} else
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
		else if (_tsb->s_flags & MS_RDONLY)                          \
			(result) = -EROFS;                                   \
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
		else if ((_osb->s_flags | _nsb->s_flags) & MS_RDONLY)       \
			(result) = -EROFS;                                  \
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
	vfs_namespace_changed();
	VFS_PATH_RESULT(ret, mkdir, mode);
	vfs_namespace_changed();
	if (!ret)
		inotify_created(sb, path);
	return ret;
}

int vfs_rmdir(super_block *sb, const char *path)
{
	inotify_node *node = inotify_snapshot(sb, path);
	int ret;
	vfs_namespace_changed();
	VFS_PATH_RESULT(ret, rmdir);
	vfs_namespace_changed();
	if (!ret)
		inotify_removed(node);
	inotify_snapshot_put(node);
	return ret;
}

int vfs_unlink(super_block *sb, const char *path)
{
	inotify_node *node = inotify_snapshot(sb, path);
	int ret;
	vfs_namespace_changed();
	VFS_PATH_RESULT(ret, unlink);
	vfs_namespace_changed();
	if (!ret)
		inotify_removed(node);
	inotify_snapshot_put(node);
	return ret;
}

int vfs_link(super_block *sb, const char *oldpath, const char *newpath)
{
	inotify_node *node = inotify_snapshot(sb, oldpath);
	int ret;
	vfs_namespace_changed();
	VFS_PATH2_RESULT(ret, link);
	vfs_namespace_changed();
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
	vfs_namespace_changed();
	VFS_PATH2_RESULT(ret, rename);
	vfs_namespace_changed();
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
	if (target_sb->s_flags & MS_RDONLY)
		return -EROFS;
	if (!target_sb->s_op || !target_sb->s_op->symlink)
		return -ENOSYS;
	vfs_namespace_changed();
	int ret = target_sb->s_op->symlink(target_sb, target, rel_path);
	vfs_namespace_changed();
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

int vfs_mknod(super_block *sb, const char *path, unsigned mode, unsigned dev)
{
	super_block *node_sb;
	int ret;

	if (!sb || !path || !*path)
		return -EINVAL;

	super_block *parent_sb;
	char *suffix, *parent = name_get();
	if (!parent)
		return -ENOMEM;
	strcpy(parent, path);
	char *slash = strrchr(parent, '/');
	if (slash == parent)
		parent[1] = 0;
	else if (slash)
		*slash = 0;
	int readonly = sb_path_resolve(sb, parent, &parent_sb, &suffix) &&
		       (parent_sb->s_flags & MS_RDONLY);
	name_put(parent);
	if (readonly)
		return -EROFS;

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
	if (!target_sb->s_op || !target_sb->s_op->statfs) {
		/* Synthetic device entries use the containing filesystem's metadata. */
		char *parent = name_get();
		if (!parent)
			return -ENOMEM;
		strcpy(parent, path);
		size_t length = strlen(parent);
		while (length > 1 && parent[length - 1] == '/')
			parent[--length] = 0;
		char *slash = strrchr(parent, '/');
		if (!slash || (slash == parent && !parent[1])) {
			name_put(parent);
			return -ENOSYS;
		}
		if (slash == parent)
			parent[1] = 0;
		else
			*slash = 0;
		ret = vfs_statfs(sb, parent, buf);
		name_put(parent);
		return ret;
	}
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

	if ((target_sb->s_flags & MS_RDONLY) && !(flag & O_PATH) &&
	    ((flag & O_ACCMODE) != O_RDONLY || (flag & (O_CREAT | O_TRUNC)))) {
		file *metadata = (!*rel_path || !strcmp(rel_path, "/")) ?
					 (target_sb->s_op->open_root ?
						  target_sb->s_op->open_root(
							  target_sb, O_PATH) :
						  NULL) :
					 (target_sb->s_op->open ?
						  target_sb->s_op->open(
							  target_sb, rel_path,
							  O_PATH | O_NOFOLLOW) :
						  NULL);
		unsigned mode = metadata && metadata->f_inode ?
					metadata->f_inode->i_mode :
					0;
		if (metadata)
			fs_put_file(metadata);
		if (!S_ISCHR(mode) && !S_ISBLK(mode) && !S_ISFIFO(mode) &&
		    !S_ISLNK(mode)) {
			if (current->execution)
				current->execution->fs_open_error = -EROFS;
			return NULL;
		}
	}

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
		if (flag & O_CREAT)
			vfs_namespace_changed();
		if (link_target && target_sb->s_op->open_link)
			fp = target_sb->s_op->open_link(target_sb, rel_path,
							flag, link_target);
		else
			fp = target_sb->s_op->open(target_sb, rel_path, flag);
		if (flag & O_CREAT)
			vfs_namespace_changed();
		if (fp) {
			vfs_set_file_origin(fp, target_sb, rel_path);
			fp->f_mount_flags = target_sb->s_flags;
		}
		return fp;
	}

	return NULL;
}

static void vfs_file_mount_identity(file *fp, super_block *sb,
				    const char *path)
{
	if (!fp || fp->f_mount_id)
		return;
	while (sb) {
		if (sb->s_fstype[0]) {
			fp->f_mount_id = sb->s_mount_id;
			fp->f_mount_root = !*path || !strcmp(path, "/");
		}
		size_t length;
		mutex_lock(&sb->s_lock);
		vfs_mount_node *mount = sb_mount_prefix(sb, path, &length);
		super_block *child = mount ? mount->sb : NULL;
		mutex_unlock(&sb->s_lock);
		if (!child)
			break;
		sb = child;
		path += length;
	}
}

/* Follow final symlinks through the mount tree, including ext4-to-proc links. */
file *vfs_open(super_block *sb, const char *path, int flag)
{
	if (current->execution)
		current->execution->fs_open_error = 0;
	file *fp;
	char *target = NULL, *joined = NULL;
	const char *lookup = path;
	super_block *lookup_sb = sb;
	unsigned depth;
	task_execution *execution = current->fs ? current->execution : NULL;
	uint64_t generation = vfs_namespace_generation();
	char cached_target[256];
	int cacheable = 1;

	if (!sb || !path)
		return NULL;
	if (flag & O_NOFOLLOW) {
		fp = vfs_open_raw(sb, path, flag, NULL);
		vfs_file_mount_identity(fp, sb, path);
		return fp;
	}
	if (execution && sb == current->fs->root && !(flag & O_CREAT) &&
	    execution->link_cache_root == sb &&
	    execution->link_cache_generation == generation &&
	    !strcmp(execution->link_cache_path, path)) {
		strcpy(cached_target, execution->link_cache_target);
		lookup = cached_target;
	}
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
		/* procfs links such as cwd/exe/fd are dynamic without a directory edit. */
		cacheable = 0;
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
	if (fp && cacheable && execution && sb == current->fs->root &&
	    lookup != path && lookup != cached_target && !(flag & O_CREAT) &&
	    strlen(path) < sizeof(execution->link_cache_path) &&
	    strlen(lookup) < sizeof(execution->link_cache_target) &&
	    generation == vfs_namespace_generation()) {
		strcpy(execution->link_cache_path, path);
		strcpy(execution->link_cache_target, lookup);
		execution->link_cache_root = sb;
		execution->link_cache_generation = generation;
	}
	vfs_file_mount_identity(fp, lookup_sb, lookup);
	if (fp && !fp->f_name)
		fp->f_name = strdup(lookup);
	if (joined)
		name_put(joined);
	if (target)
		name_put(target);
	return fp;
}
