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
static void kmsg_register(super_block *sb)
{
	vfs_mount(sb, "/kmsg", sget(&kmsg_ops));
}
PROC_INIT(kmsg_register);
