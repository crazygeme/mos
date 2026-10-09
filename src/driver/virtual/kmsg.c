#include <fs/entries.h>
#include <driver/driver.h>
#include <device/devnode.h>
#include <fs/syslog.h>
#include <device/devnums.h>

static file *kmsg_open(super_block *sb, unsigned rdev, int flag)
{
	(void)sb;
	(void)flag;
	return syslog_open(S_IFCHR | 0600, rdev);
}

static void kmsg_register(void)
{
	
	vfs_entry_device(devfs_entries(), "/kmsg", S_IFCHR | 0600, MKDEV(KMSG_MAJOR, KMSG_MINOR), "mem", kmsg_open);
}

static int kmsg_register_probe(void)
{
	kmsg_register();
	return 0;
}

static driver_t kmsg_register_driver = {
	.name = "kmsg",
	.bus = DEVICE_BUS_VIRTUAL,
	.virtual_id = VDEV_KMSG,
	.probe_virtual = kmsg_register_probe,
};
DRIVER_REGISTER(kmsg_register_driver);
