#include <fs/sysfs.h>
#include <fs/mount.h>
#include <macro.h>
#include <lib/klib.h>

static vfs_entry_tree *sys_tree;

vfs_entry_tree *device_sysfs_entries(void)
{
	if (!sys_tree)
		sys_tree = vfs_entry_tree_create();
	return sys_tree;
}

static super_block *sysfs_get_sb(const char *dev, const char *target, int flags,
				 void *data)
{
	if (!sys_tree || vfs_entry_tree_error(sys_tree))
		return NULL;
	return vfs_entry_tree_mount(sys_tree);
}

static fs_type sysfs_type = { .name = "sysfs", .get_sb = sysfs_get_sb };

static void sysfs_register(void)
{
	if (!device_sysfs_entries())
		return;
	if (!vfs_entry_tree_error(sys_tree))
		fs_register_type(&sysfs_type);
}

/* Export the device inventory after hardware initialization. */
KERNEL_INIT(5, sysfs_register);
