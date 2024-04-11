#include "mm.h"
#include <klib.h>
#include <errno.h>
#include <rbtree.h>
#include <mount.h>
#include <lock.h>
#include <fs.h>
#include <debugfs.h>
static int mount_path_comp(void *k1, void *k2)
{
	const char *left = k1;
	const char *right = k2;
	return 0 - strcmp(left, right);
}

static int mount_path_resolve(mount_point *mp, const char *path,
			      mount_point **out_mp, char **out_path)
{
	key_value_pair *kv = NULL;
	mount_point *new_mp = NULL;
	const char *new_path = NULL;

	if (!mp || !path)
		return 0;

	if (*path == '\0') {
		goto done;
	}

	mutex_lock(&mp->lock);

	for (kv = hash_first(mp->folders); kv;
	     kv = hash_next(mp->folders, kv)) {
		if (strstr(path, kv->key) == path) {
			mutex_unlock(&mp->lock);

			new_path = path + strlen(kv->key);
			new_mp = kv->val;
			return mount_path_resolve(new_mp, new_path, out_mp,
						  out_path);
		}
	}

	mutex_unlock(&mp->lock);

done:
	*out_mp = mp;
	*out_path = path;
	return 1;
}

mount_point *mount_create()
{
	mount_point *ret = kmalloc(sizeof(*ret));
	mutex_init(&ret->lock);
	ret->folders = hash_create(mount_path_comp);
	ret->files = hash_create(mount_path_comp);
	ret->ref = 1;
	return ret;
}

void mount_ref(mount_point *mp)
{
	__sync_add_and_fetch(&mp->ref, 1);
}

void mount_deref(mount_point *mp)
{
	key_value_pair *kv = NULL;
	key_value_pair *next = NULL;
	debug_inode *inode = NULL;

	if (__sync_add_and_fetch(&mp->ref, -1) == 0) {
		mutex_lock(&mp->lock);

		for (kv = hash_first(mp->folders); kv;
		     kv = hash_next(mp->folders, kv)) {
			kfree(kv->key);
			mount_deref(kv->val);
		}

		hash_destroy(mp->folders);

		for (kv = hash_first(mp->files); kv;
		     kv = hash_next(mp->files, kv)) {
			kfree(kv->key);
			inode = kv->val;
			vm_free(inode->buf, 1);
			kfree(inode);
		}

		hash_destroy(mp->files);

		mutex_unlock(&mp->lock);

		kfree(mp);
	}
}

int do_mount(mount_point *mp, const char *path, const mount_op *op)
{
	const char *new_path = NULL;
	mount_point *new_mp = NULL;
	mount_point *child_mp = NULL;
	char *key = NULL;

	if (!mp || !path) {
		return NULL;
	}

	if (*path != '/') {
		return EINVAL;
	}

	if (!mount_path_resolve(mp, path, &new_mp, &new_path)) {
		return NULL;
	}

	if (new_mp == mp) {
		if (*new_path == '\0')
			return EINVAL;

		mutex_lock(&new_mp->lock);
		if (hash_find(new_mp->folders, path)) {
			mutex_unlock(&new_mp->lock);
			return EEXIST;
		}

		child_mp = mount_create();
		child_mp->op = *op;
		key = strdup(new_path);
		hash_insert(new_mp->folders, key, child_mp);
		mutex_unlock(&new_mp->lock);
		return 0;
	}

	return do_mount(new_mp, new_path, op);
}

int do_unmount(mount_point *mp, const char *path)
{
	const char *new_path = NULL;
	mount_point *new_mp = NULL;
	char *key = NULL;
	key_value_pair *kv = NULL;

	if (!mp || !path) {
		return NULL;
	}

	if (*path != '/') {
		return EINVAL;
	}

	if (!mount_path_resolve(mp, path, &new_mp, &new_path)) {
		return NULL;
	}

	if (new_mp == mp) {
		if (*new_path != '\0') {
			return EEXIST;
		}

		mutex_lock(&new_mp->lock);
		kv = hash_find(new_mp->folders, path);

		if (!kv) {
			mutex_unlock(&new_mp->lock);
			return EEXIST;
		}

		hash_remove(new_mp->folders, path);
		mount_deref(kv->val);
		kfree(kv->key);
		mutex_unlock(&new_mp->lock);
		return 0;
	}

	return do_unmount(new_mp, new_path);
}

filep mount_open(mount_point *mp, const char *path, int flag, const char *mode)
{
	const char *new_path = NULL;
	mount_point *new_mp = NULL;
	filep fp = NULL;
	filep newfp = NULL;
	key_value_pair *kv = NULL;

	if (!mp || !path) {
		return NULL;
	}

	if (!mount_path_resolve(mp, path, &new_mp, &new_path)) {
		return NULL;
	}

	// recursivly open mount point
	if (new_mp != mp) {
		return mount_open(new_mp, new_path, flag, mode);
	}

	// open self
	if (*new_path == '\0' ||
	    (*new_path == '/' && *(new_path + 1) == '\0')) {
		// open self
		if (!new_mp->op.alloc)
			return NULL;

		fp = new_mp->op.alloc(new_mp);
		fs_refrence(fp);
		return fp;
	}

	mutex_lock(&new_mp->lock);
	kv = hash_find(new_mp->files, new_path);
	mutex_unlock(&new_mp->lock);

	if (kv) {
		fp = debugfs_open(kv->val);
		fs_refrence(fp);
		return fp;
	}

	if (!new_mp->op.alloc)
		return NULL;

	fp = new_mp->op.alloc(new_mp);
	fs_refrence(fp);
	newfp = fp->op.open(fp->inode, new_path, flag, mode);
	fs_destroy(fp);
	if (newfp)
		fs_refrence(newfp);
	return newfp;
}

int mount_add_file(mount_point *mp, const char *path,
		   void (*fill)(void *buf, size_t size))
{
	const char *new_path = NULL;
	mount_point *new_mp = NULL;
	key_value_pair *kv = NULL;
	char *key = NULL;
	debug_inode *val = NULL;

	if (!mp || !path) {
		return -1;
	}

	if (!mount_path_resolve(mp, path, &new_mp, &new_path)) {
		return -1;
	}

	if (*new_path != '/') {
		return EINVAL;
	}

	if (strchr(new_path + 1, '/') != NULL) {
		return EINVAL;
	}

	mutex_lock(&new_mp->lock);

	kv = hash_find(new_mp->files, new_path);
	if (kv != NULL) {
		mutex_unlock(&new_mp->lock);
		return EEXIST;
	}

	key = strdup(new_path);
	val = malloc(sizeof(*val));
	val->buf = NULL;
	val->fill = fill;

	hash_insert(new_mp->files, key, val);

	mutex_unlock(&new_mp->lock);
}
