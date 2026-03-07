#include <mm.h>
#include <klib.h>
#include <errno.h>
#include <rbtree.h>
#include <mount.h>
#include <lock.h>
#include <fs.h>
#include <debugfs.h>

static int sb_path_comp(void *k1, void *k2)
{
	const char *left = k1;
	const char *right = k2;
	return 0 - strcmp(left, right);
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
	key_value_pair *kv;
	super_block *child;
	const char *rest;
	size_t klen;

	if (!sb || !path)
		return 0;

	if (*path == '\0')
		goto done;

	mutex_lock(&sb->s_lock);

	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv)) {
		klen = strlen(kv->key);
		if (strncmp(path, kv->key, klen) == 0 &&
		    (path[klen] == '/' || path[klen] == '\0')) {
			child = kv->val;
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
	super_block *sb = calloc(1, sizeof(*sb));
	mutex_init(&sb->s_lock);
	sb->s_mounts = hash_create(sb_path_comp);
	sb->s_files = hash_create(sb_path_comp);
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
	key_value_pair *kv;
	debug_inode *di;

	if (__sync_add_and_fetch(&sb->s_ref, -1) != 0)
		return;

	mutex_lock(&sb->s_lock);

	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv)) {
		kfree(kv->key);
		sb_put(kv->val);
	}
	hash_destroy(sb->s_mounts);

	for (kv = hash_first(sb->s_files); kv;
	     kv = hash_next(sb->s_files, kv)) {
		kfree(kv->key);
		di = kv->val;
		if (di->buf)
			vm_free(di->buf, 1);
		kfree(di);
	}
	hash_destroy(sb->s_files);

	mutex_unlock(&sb->s_lock);

	if (sb->s_op && sb->s_op->put_super)
		sb->s_op->put_super(sb);

	kfree(sb);
}

int vfs_mount(super_block *sb, const char *path, const super_operations *s_op)
{
	key_value_pair *kv;
	super_block *child;
	char *key;
	size_t klen;

	if (!sb || !path || !s_op)
		return -EINVAL;

	if (*path != '/')
		return -EINVAL;

	mutex_lock(&sb->s_lock);

	/* Reject duplicate direct registration */
	if (hash_find(sb->s_mounts, path)) {
		mutex_unlock(&sb->s_lock);
		return -EEXIST;
	}

	/* Delegate to an existing child that is a strict prefix of path */
	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv)) {
		klen = strlen(kv->key);
		if (strncmp(path, kv->key, klen) == 0 &&
		    (path[klen] == '/' || path[klen] == '\0')) {
			child = kv->val;
			mutex_unlock(&sb->s_lock);
			return vfs_mount(child, path + klen, s_op);
		}
	}

	/* No prefix match: register as a direct child */
	child = sget(s_op);
	key = strdup(path);
	hash_insert(sb->s_mounts, key, child);
	mutex_unlock(&sb->s_lock);
	return 0;
}

int vfs_umount(super_block *sb, const char *path)
{
	key_value_pair *kv;
	super_block *child;
	size_t klen;

	if (!sb || !path || *path != '/')
		return -EINVAL;

	mutex_lock(&sb->s_lock);

	/* Direct child mount? */
	kv = hash_find(sb->s_mounts, path);
	if (kv) {
		child = kv->val;
		hash_remove_at(sb->s_mounts, kv);
		mutex_unlock(&sb->s_lock);
		kfree(kv->key);
		sb_put(child);
		return 0;
	}

	/* Prefix match: delegate to child */
	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv)) {
		klen = strlen(kv->key);
		if (strncmp(path, kv->key, klen) == 0 &&
		    (path[klen] == '/' || path[klen] == '\0')) {
			child = kv->val;
			mutex_unlock(&sb->s_lock);
			return vfs_umount(child, path + klen);
		}
	}

	mutex_unlock(&sb->s_lock);
	return -ENOENT;
}

/*
 * sb_open_inode - wrap an inode in a new open file struct.
 * The returned file takes ownership of the inode (freed via f_op->release).
 */
static file *sb_open_inode(inode *node, int flag)
{
	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_op = node->i_fop;
	fp->f_count = 1;
	fp->f_mode = node->i_mode;
	fp->f_pos = 0;
	return fp;
}

file *vfs_open(super_block *sb, const char *path, int flag)
{
	super_block *target_sb;
	char *rel_path;
	key_value_pair *kv;
	inode *node;

	if (!sb || !path)
		return NULL;

	if (!sb_path_resolve(sb, path, &target_sb, &rel_path))
		return NULL;

	/* sb_path_resolve descends into children; if target differs, re-enter */
	if (target_sb != sb)
		return vfs_open(target_sb, rel_path, flag);

	/* Opening the mount root (empty path suffix means we landed here) */
	if (*rel_path == '\0') {
		if (!target_sb->s_op || !target_sb->s_op->get_root)
			return NULL;
		node = target_sb->s_op->get_root(target_sb);
		if (!node)
			return NULL;
		return sb_open_inode(node, flag);
	}

	/* Look up a registered pseudo-file */
	mutex_lock(&target_sb->s_lock);
	kv = hash_find(target_sb->s_files, rel_path);
	mutex_unlock(&target_sb->s_lock);

	if (kv)
		return debugfs_open(kv->val);

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
	if (target_sb->s_op && target_sb->s_op->get_root) {
		node = target_sb->s_op->get_root(target_sb);
		if (!node)
			return NULL;
		if (!node->i_fop) {
			free(node);
			return NULL;
		}
		return sb_open_inode(node, flag);
	}

	return NULL;
}

int vfs_create_file(super_block *sb, const char *path,
		    void (*fill)(void *buf, size_t size))
{
	super_block *target_sb;
	char *rel_path;
	key_value_pair *kv;
	char *key;
	debug_inode *val;

	if (!sb || !path)
		return -EINVAL;

	if (!sb_path_resolve(sb, path, &target_sb, &rel_path))
		return -EINVAL;

	/* rel_path must be a single filename component, e.g. "/meminfo" */
	if (*rel_path != '/')
		return -EINVAL;

	if (strchr(rel_path + 1, '/') != NULL)
		return -EINVAL;

	mutex_lock(&target_sb->s_lock);
	kv = hash_find(target_sb->s_files, rel_path);
	if (kv) {
		mutex_unlock(&target_sb->s_lock);
		return -EEXIST;
	}

	key = strdup(rel_path);
	val = malloc(sizeof(*val));
	val->buf = NULL;
	val->fill = fill;

	hash_insert(target_sb->s_files, key, val);
	mutex_unlock(&target_sb->s_lock);
	return 0;
}
