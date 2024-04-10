#include <klib.h>
#include <errno.h>
#include <rbtree.h>
#include <mount.h>
#include <lock.h>
#include <fs.h>

typedef struct _mount_point {
	mount_op op;
	hash_table *mounts;
	cond_t lock;
	unsigned ref;
} mount_point;

static int mount_path_comp(void *k1, void *k2)
{
	const char *left = k1;
	const char *right = k2;
	return 0 - strcmp(left, right);
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
	mount_point *new_mp = NULL;
	char *key = NULL;
	if (!path || !*path) {
		return EINVAL;
	}

	if (*path != '/') {
		return EINVAL;
	}

	cond_wait(&mp->lock);
	if (hash_find(mp->mounts, path)) {
		cond_notify(&mp->lock);
		return EEXIST;
	}

	new_mp = mount_create();
	new_mp->op = *op;
	key = strdup(path);
	hash_insert(mp->mounts, key, new_mp);
	cond_notify(&mp->lock);
}

int do_unmount(mount_point *mp, const char *path)
{
	key_value_pair *kv = NULL;

	if (!path || !*path) {
		return EINVAL;
	}

	if (*path != '/') {
		return EINVAL;
	}

	cond_wait(&mp->lock);
	kv = hash_find(mp->mounts, path);

	if (!kv) {
		cond_notify(&mp->lock);
		return EEXIST;
	}

	hash_remove(mp->mounts, path);
	mount_deref(kv->val);
	kfree(kv->key);
	cond_notify(&mp->lock);
}

filep mount_open(mount_point *mp, const char *path)
{
	key_value_pair *kv = NULL;
	const char *new_path = NULL;
	mount_point *new_mp = NULL;
	filep fp = NULL;

	if (!mp || !path) {
		return NULL;
	}

	cond_wait(&mp->lock);

	for (kv = hash_first(mp->mounts); kv; kv = hash_next(mp->mounts, kv)) {
		if (strstr(path, kv->key) == path) {
			new_path = path + strlen(kv->key);
			new_mp = kv->val;
			cond_notify(&mp->lock);

			if (strlen(new_path) == 0) {
				// open self
				fp = new_mp->op.alloc();
				fp->name = strdup(path);
				fs_refrence(fp);
				return fp;
			}

			return mount_open(new_mp, new_path);
		}
	}

	cond_notify(&mp->lock);
	return NULL;
}