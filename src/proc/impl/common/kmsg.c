#include <proc/proc.h>
#include <fs/syslog.h>

static file *kmsg_open(super_block *sb, int flag)
{
	(void)sb;
	(void)flag;
	return syslog_open(S_IFREG | 0400, 0);
}
static const super_operations kmsg_ops = {
	.open_root = kmsg_open,
};
static void kmsg_register(void)
{
	vfs_entry_node *root = procfs_entries();
	if (!root)
		return;
	vfs_entry_mount(root, "kmsg", S_IFREG | 0400, sget(&kmsg_ops));
}
KERNEL_INIT(4, kmsg_register);
