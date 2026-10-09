#include <fs/entries.h>
#include <driver/driver.h>
#include <fs/vfs.h>
#include <device/devnode.h>
#include <macro.h>
#include <unistd.h>

/*
 * src/dev/initctl.c - /dev/initctl named FIFO
 *
 * A persistent named pipe shared across all opens of /dev/initctl.
 * Writers push init requests; the init process reads them.
 *
 * The underlying cyclebuf is owned by the devnode superblock and persists
 * across open/close cycles.  All FIFO semantics (EOF on last writer close,
 * EPIPE when no readers, blocking read) are handled by devnode.c.
 */

static void initctl_dev_register(void)
{
	vfs_entry_device(devfs_entries(), "/initctl", S_IFIFO | 0600, 0, NULL, NULL);
}

static int initctl_dev_register_probe(void)
{
	initctl_dev_register();
	return 0;
}

static driver_t initctl_dev_register_driver = {
	.name = "initctl",
	.bus = DEVICE_BUS_VIRTUAL,
	.virtual_id = VDEV_INITCTL,
	.probe_virtual = initctl_dev_register_probe,
};
DRIVER_REGISTER(initctl_dev_register_driver);
