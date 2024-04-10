#include <klib.h>
#include <errno.h>
#include <rbtree.h>
#include <mount.h>
#include <lock.h>
#include <fs.h>

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

	cond_wait(&mp->lock);

	for (kv = hash_first(mp->mounts); kv; kv = hash_next(mp->mounts, kv)) {
		if (strstr(path, kv->key) == path) {
			cond_notify(&mp->lock);

			new_path = path + strlen(kv->key);
			new_mp = kv->val;
			return mount_path_resolve(new_mp, new_path, out_mp,
						  out_path);
		}
	}

	cond_notify(&mp->lock);

done:
	*out_mp = mp;
	*out_path = path;
	return 1;
}

mount_point *mount_create()
{
	mount_point *ret = kmalloc(sizeof(*ret));
	cond_init(&ret->lock, "mount_lock", 0);
	ret->mounts = hash_create(mount_path_comp);
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

	if (__sync_add_and_fetch(&mp->ref, -1) == 0) {
		cond_wait(&mp->lock);

		for (kv = hash_first(mp->mounts); kv;
		     kv = hash_next(mp->mounts, kv)) {
			kfree(kv->key);
			mount_deref(kv->val);
		}

		hash_destroy(mp->mounts);

		cond_notify(&mp->lock);

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

		cond_wait(&new_mp->lock);
		if (hash_find(new_mp->mounts, path)) {
			cond_notify(&new_mp->lock);
			return EEXIST;
		}

		child_mp = mount_create();
		child_mp->op = *op;
		key = strdup(new_path);
		hash_insert(new_mp->mounts, key, child_mp);
		cond_notify(&new_mp->lock);
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

		cond_wait(&new_mp->lock);
		kv = hash_find(new_mp->mounts, path);

		if (!kv) {
			cond_notify(&new_mp->lock);
			return EEXIST;
		}

		hash_remove(new_mp->mounts, path);
		mount_deref(kv->val);
		kfree(kv->key);
		cond_notify(&new_mp->lock);
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

	if (!mp || !path) {
		return NULL;
	}

	if (!mount_path_resolve(mp, path, &new_mp, &new_path)) {
		return NULL;
	}

	if (new_mp == mp) {
		// open self
		if (!new_mp->op.alloc)
			return NULL;

		fp = new_mp->op.alloc(new_mp);

		// a path under this mount point, let target mount point decide
		// how to open the file.
		if (strlen(new_path) != 0) {
			newfp = fp->op.open(new_mp, new_path, flag, mode);
			fs_destroy(fp);
			return newfp;
		}

		// open self as a dir
		fp->name = strdup(path);
		fs_refrence(fp);
		return fp;
	}

	return mount_open(new_mp, new_path, flag, mode);
}