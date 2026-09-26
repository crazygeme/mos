#include <dev/dev.h>
#include <fs/syslog.h>
#include "devnums.h"

static file *kmsg_open(super_block *sb, unsigned rdev, int flag)
{
	(void)sb;
	(void)flag;
	return syslog_open(S_IFCHR | 0600, rdev);
}

static void kmsg_register(super_block *sb)
{
	cdev_register_named(S_IFCHR, KMSG_MAJOR, KMSG_MINOR, 1, "mem", kmsg_open);
	vfs_mknod(sb, "/kmsg", S_IFCHR | 0600, MKDEV(KMSG_MAJOR, KMSG_MINOR));
}
DEV_INIT(kmsg_register);
