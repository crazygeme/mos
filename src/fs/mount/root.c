#include <fs/fs.h>
#include <fs/mount.h>
#include <fs/super/ext4.h>
#include <ps/ps.h>
#include <ext4.h>
#include <hw/hdd.h>

#include <macro.h>

static void fs_mount_root()
{
	task_struct *cur = CURRENT_TASK();
	cur->root = ext4_get();
	ext4_mount(hdd_first_dev_name, "/", 0);
	ext4_cache_write_back("/", true);
}

KERNEL_INIT(3, fs_mount_root);
