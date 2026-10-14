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

struct mountinfo_context {
	proc_buf_t *pb;
	unsigned id;
};
static void emit_mountinfo(const super_block *sb, void *arg)
{
	struct mountinfo_context *c = arg;
	unsigned id = ++c->id;
	proc_buf_printf(c->pb, "%u 1 0:%u / %s %s - %s %s %s\n", id, id,
			sb->s_mountpoint,
			(sb->s_flags & MS_RDONLY) ? "ro" : "rw", sb->s_fstype,
			sb->s_devname,
			!strcmp(sb->s_fstype, "cgroup") ? sb->s_devname :
			(sb->s_flags & MS_RDONLY)	? "ro" :
							  "rw");
}
void proc_fill_mounts(proc_buf_t *pb, task_struct *task, int info)
{
	if (info) {
		struct mountinfo_context c = { pb, 1 };
		proc_buf_printf(pb, "1 0 0:1 / / rw - ext4 /dev/hda1 rw\n");
		vfs_mount_walk(task->fs->root, emit_mountinfo, &c);
	} else {
		proc_buf_printf(pb, "rootfs / rootfs rw 0 0\n");
		vfs_mount_walk(task->fs->root, emit_mount, pb);
	}
}
static void fill(proc_buf_t *pb)
{
	const char *rootfs_opts =
		(current->fs->root->s_flags & MS_RDONLY) ? "ro" : "rw";

	proc_buf_printf(pb, "rootfs / rootfs %s 0 0\n", rootfs_opts);
	vfs_mount_walk(current->fs->root, emit_mount, pb);
}

DEFINE_PROC_FILE(mounts, fill);
