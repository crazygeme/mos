#ifndef MOS_DEVICE_SYSFS_H
#define MOS_DEVICE_SYSFS_H
#include <fs/entries.h>

/* Shared filesystem view; buses and drivers publish their own attributes. */
vfs_entry_tree *device_sysfs_entries(void);
#endif
