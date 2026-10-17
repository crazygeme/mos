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
#include <device/blockdev.h>
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
	super_block *root;
};
struct parent_context {
	const super_block *mount;
	const super_block *parent;
	size_t length;
};
static void find_parent(const super_block *candidate, void *opaque)
{
	struct parent_context *c = opaque;
	const char *path = c->mount->s_mountpoint;
	size_t length = strlen(candidate->s_mountpoint);
	if (candidate != c->mount && length > c->length &&
	    !strncmp(path, candidate->s_mountpoint, length) &&
	    (length == 1 || path[length] == '/')) {
		c->parent = candidate;
		c->length = length;
	}
}
static void emit_mountinfo(const super_block *sb, void *arg)
{
	struct mountinfo_context *c = arg;
	struct parent_context parent = { .mount = sb };
	vfs_mount_walk(c->root, find_parent, &parent);
	blockdev_info device;
	unsigned major = 0, minor = 0;
	if (!strncmp(sb->s_devname, "/dev/", 5) && blockdev_lookup(sb->s_devname + 5, &device)) {
		major = device.major;
		minor = device.minor;
	}
	char optional[96] = "";
	if (sb->s_peer_group)
		sprintf(optional + strlen(optional), " shared:%u", sb->s_peer_group);
	if (sb->s_master_group)
		sprintf(optional + strlen(optional), " master:%u", sb->s_master_group);
	if (sb->s_unbindable)
		strcat(optional, " unbindable");
	proc_buf_printf(c->pb, "%u %u %u:%u / %s %s%s - %s %s %s\n",
			sb->s_mount_id, parent.parent ? parent.parent->s_mount_id : sb->s_mount_id, major, minor,
			sb->s_mountpoint,
			(sb->s_flags & MS_RDONLY) ? "ro" : "rw", optional, sb->s_fstype,
			sb->s_devname,
			!strcmp(sb->s_fstype, "cgroup") ? sb->s_devname :
			(sb->s_flags & MS_RDONLY) ? "ro" : "rw");
}
void proc_fill_mounts_root(proc_buf_t *pb, super_block *root, int info)
{
	if (info) {
		struct mountinfo_context c = { pb, root };
		vfs_mount_walk(root, emit_mountinfo, &c);
	} else {
		vfs_mount_walk(root, emit_mount, pb);
	}
}
void proc_fill_mounts(proc_buf_t *pb, task_struct *task, int info)
{
	proc_fill_mounts_root(pb, task->fs->root, info);
}
static void fill(proc_buf_t *pb)
{
	vfs_mount_walk(current->fs->root, emit_mount, pb);
}

DEFINE_PROC_FILE(mounts, fill);
