#ifndef _MOUNT_H
#define _MOUNT_H

#include <rbtree.h>
#include <fs.h>
#include <lock.h>

typedef struct _mount_point mount_point;

typedef struct _mount_op {
	inode *(*get_inode)(mount_point *mp);
} mount_op;

struct _mount_point {
	mount_op op;
	hash_table *folders;
	hash_table *files;
	mutex_t lock;
	unsigned ref;
};

mount_point *mount_create();

void mount_ref(mount_point *mp);

void mount_deref(mount_point *mp);

file * mount_open(mount_point *m, const char *path, int flag, const char *mode);

int do_mount(mount_point *m, const char *path, const mount_op *op);

int do_unmount(mount_point *m, const char *path);

int mount_add_file(mount_point *m, const char *path,
		   void (*fill)(void *buf, size_t size));

#endif