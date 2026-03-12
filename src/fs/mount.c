#include <mm/mm.h>
#include <fs/mount.h>
#include <fs/fs.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <lib/lock.h>
#include <errno.h>

/**
 * To make sure sorted by path
 */
static int sb_path_comp(void *k1, void *k2)
{
	const char *left = k1;
	const char *right = k2;
	return 0 - strcmp(left, right);
}

/*
 * Release memory for `key`
 */
static void sb_entry_evict(const key_value_pair *pair)
{
	free(pair->key);
	sb_put(pair->val);
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
	sb->s_mounts = hash_create(sb_path_comp, sb_entry_evict);
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

	if (__sync_add_and_fetch(&sb->s_ref, -1) != 0)
		return;

	mutex_lock(&sb->s_lock);

	hash_destroy(sb->s_mounts);

	mutex_unlock(&sb->s_lock);

	if (sb->s_op && sb->s_op->release)
		sb->s_op->release(sb);
	else
		kfree(sb);
}

int vfs_mount(super_block *sb, const char *path, super_block *next)
{
	key_value_pair *kv;
	super_block *child;
	char *key;
	size_t klen;

	if (!sb || !path || !next)
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
			return vfs_mount(child, path + klen, next);
		}
	}

	/* No prefix match: register as a direct child */
	child = next;
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
		if (!target_sb->s_op || !target_sb->s_op->open_root)
			return NULL;
		return target_sb->s_op->open_root(target_sb);
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
		return target_sb->s_op->open_root(target_sb);
	}

	return NULL;
}
