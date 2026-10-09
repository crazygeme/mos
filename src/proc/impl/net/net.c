#include "proc_net.h"
#include <proc/proc.h>
#include <fs/entries.h>

static file *net_dev_open(super_block *sb, int flags)
{
	(void)sb;
	(void)flags;
	return open_net_dev();
}
static const super_operations net_dev_ops = { .open_root = net_dev_open };

static file *net_if_inet6_open(super_block *sb, int flags)
{
	(void)sb;
	(void)flags;
	return open_net_if_inet6();
}
static const super_operations net_if_inet6_ops = { .open_root =
							   net_if_inet6_open };

static file *net_route_open(super_block *sb, int flags)
{
	(void)sb;
	(void)flags;
	return open_net_route();
}
static const super_operations net_route_ops = { .open_root = net_route_open };

static file *net_arp_open(super_block *sb, int flags)
{
	(void)sb;
	(void)flags;
	return open_net_arp();
}
static const super_operations net_arp_ops = { .open_root = net_arp_open };

static void proc_net_register(void)
{
	vfs_entry_node *root = procfs_entries();
	if (!root)
		return;
	vfs_entry_node *net = vfs_entry_directory(root, "net");
	vfs_entry_mount(net, "dev", S_IFREG | 0444, sget(&net_dev_ops));
	vfs_entry_mount(net, "if_inet6", S_IFREG | 0444,
			sget(&net_if_inet6_ops));
	vfs_entry_mount(net, "route", S_IFREG | 0444, sget(&net_route_ops));
	vfs_entry_mount(net, "arp", S_IFREG | 0444, sget(&net_arp_ops));
}
KERNEL_INIT(4, proc_net_register);
