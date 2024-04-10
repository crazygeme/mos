#ifndef _DEBUGFS_H
#define _DEBUGFS_H

#include <mount.h>

typedef struct _debugfs_entry {
	const mount_point *mp;
} debugfs_entry;

void debugfs_init();

void debugfs_add_dir(const char *name, const mount_point *entry);

void debugfs_remove_dir(const char *name);

void debugfs_add_file(const char *name);

void debugfs_remove_file(const char *name);

#endif