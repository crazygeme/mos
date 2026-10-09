#include <fs/entries.h>
#include <fs/vfs.h>
#include <device/devnode.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <macro.h>
#include <errno.h>

static vfs_entry_tree *dev_tree;

vfs_entry_node *devfs_entries(void)
{
	if (!dev_tree) {
		dev_tree = vfs_entry_tree_create();
		if (dev_tree) {
			vfs_entry_tree_type(dev_tree, 0x1373);
			vfs_entry_set_mode(vfs_entry_root(dev_tree),
					   S_IFDIR | 0755);
		}
	}
	return dev_tree ? vfs_entry_root(dev_tree) : NULL;
}

static void devfs_init(void)
{
	vfs_entry_node *root = devfs_entries();
	super_block *sb;
	if (!root)
		return;
	if (vfs_entry_tree_error(dev_tree))
		return;
	sb = vfs_entry_tree_super(dev_tree);
	strncpy(sb->s_devname, "devtmpfs", sizeof(sb->s_devname) - 1);
	strncpy(sb->s_fstype, "devtmpfs", sizeof(sb->s_fstype) - 1);
	printk("mnt: Mounting devfs on /dev\n");
	vfs_mount(CURRENT_TASK()->fs->root, "/dev", sb);
}
KERNEL_INIT(6, devfs_init);
