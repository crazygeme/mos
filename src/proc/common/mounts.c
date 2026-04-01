/*
 * /proc/mounts — list of mounted filesystems.
 *
 * Format matches Linux /proc/mounts (which is a symlink to /proc/self/mounts):
 *   device mountpoint fstype options dump pass
 *
 * "dump" and "pass" are always 0 (not used by MOS).
 */
#include <fs/vfs.h>
#include "common.h"

static void fill(proc_buf_t *pb)
{
	const mount_record *r;

	for (r = vfs_mount_list(); r; r = r->next)
		proc_buf_printf(pb, "%s %s %s %s 0 0\n", r->devname,
				r->mountpoint, r->fstype, r->options);
}

DEFINE_PROC_FILE(mounts, fill);
