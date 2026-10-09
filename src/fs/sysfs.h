#ifndef MOS_FS_SYSFS_H
#define MOS_FS_SYSFS_H
#include <fs/entries.h>

/* The filesystem owns only the shared tree and mount views. */
vfs_entry_tree *device_sysfs_entries(void);
#endif
