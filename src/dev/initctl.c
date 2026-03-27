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

#include <fs/vfs.h>
#include <dev/dev.h>
#include <macro.h>
#include <unistd.h>

static void initctl_dev_register(super_block *dev_sb)
{
	vfs_mknod(dev_sb, "/initctl", S_IFIFO | 0600, 0);
}

DEV_INIT(initctl_dev_register);
