#ifndef _MOUNT_H
#define _MOUNT_H

#include <rbtree.h>
#include <fs.h>
#include <lock.h>

typedef struct _mount_point mount_point;

typedef struct _mount_op {
	filep (*alloc)();
} mount_op;

mount_point *mount_create();

void mount_ref(mount_point *mp);

void mount_deref(mount_point *mp);

filep mount_open(mount_point *m, const char *path);

int do_mount(mount_point *m, const char *path, const mount_op *op);

int do_unmount(mount_point *m, const char *path);

#endif