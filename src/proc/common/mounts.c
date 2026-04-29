/*
 * /proc/mounts — list of mounted filesystems.
 *
 * Format matches Linux /proc/mounts (which is a symlink to /proc/self/mounts):
 *   device mountpoint fstype options dump pass
 *
 * "dump" and "pass" are always 0 (not used by MOS).
 *
 * The mount table is derived by walking the live VFS mount tree, so it always
 * reflects the current state without a separate tracking list.
 */
#include <fs/vfs.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include "common.h"

static void emit_mount(const super_block *sb, void *arg)
{
	proc_buf_t *pb = (proc_buf_t *)arg;
	const char *opts = (sb->s_flags & MS_RDONLY) ? "ro,relatime" :
						       "rw,relatime";

	proc_buf_printf(pb, "%s %s %s %s 0 0\n", sb->s_devname,
			sb->s_mountpoint, sb->s_fstype, opts);
}

static void fill(proc_buf_t *pb)
{
	const char *rootfs_opts = (current->root->s_flags & MS_RDONLY) ? "ro" :
								  "rw";

	proc_buf_printf(pb, "rootfs / rootfs %s 0 0\n", rootfs_opts);
	vfs_mount_walk(current->root, emit_mount, pb);
}

DEFINE_PROC_FILE(mounts, fill);
