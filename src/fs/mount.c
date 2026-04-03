#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <macro.h>
#include <errno.h>

/* Defined in src/dev/pts.c — returns the pre-built /dev/pts superblock. */
extern super_block *pts_get_devpts_sb(void);

/* Defined in src/fs/tmpfs.c */
extern super_block *tmpfs_get_sb(const char *dev, const char *target, int flags,
				 void *data);

/* =========================================================================
 * Filesystem type registry
 * ====================================================================== */

static fs_type *fs_type_list = NULL;

void fs_register_type(fs_type *fst)
{
	fst->next = fs_type_list;
	fs_type_list = fst;
}

static fs_type *fs_find_type(const char *name)
{
	fs_type *t;

	for (t = fs_type_list; t; t = t->next) {
		if (strcmp(t->name, name) == 0)
			return t;
	}
	return NULL;
}

/* =========================================================================
 * Minimal stub super_block for pseudo-filesystems (proc, sysfs, tmpfs …)
 *
 * open_root() returns a directory inode so that stat() on the mount point
 * succeeds.  Sub-path accesses return NULL (→ ENOENT) because no open()
 * callback is registered.
 * ====================================================================== */

static int stub_getattr(inode *node, struct stat *s)
{
	s->st_mode = node->i_mode;
	s->st_ino = node->i_ino;
	s->st_size = 0;
	s->st_blksize = 0;
	s->st_blocks = 0;
	return 0;
}

static const inode_operations stub_iops = {
	.getattr = stub_getattr,
};

static file *stub_open_root(super_block *sb, int flag)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH |
		       S_IXOTH;
	node->i_ino = 1;
	node->i_op = &stub_iops;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	return fp;
}

static super_operations stub_sops = {
	.open_root = stub_open_root,
};

static super_block *stub_get_sb(const char *dev, const char *target, int flags,
				void *data)
{
	return sget(&stub_sops);
}

/* =========================================================================
 * Built-in pseudo-filesystem registrations
 *
 * "ext4" is registered by root.c inside fs_mount_root() (KERNEL_INIT 3)
 * after lwext4 has been set up, so it intentionally does not appear here.
 * ====================================================================== */

/* "proc" is registered by src/proc/procfs.c (KERNEL_INIT 4) with a real get_sb */
/* "ext4"/"ext3" are registered by src/fs/root.c (KERNEL_INIT 3) */

static super_block *devpts_get_sb(const char *dev, const char *target,
				  int flags, void *data)
{
	return pts_get_devpts_sb();
}

static fs_type sysfs_fs_type = { .name = "sysfs", .get_sb = stub_get_sb };
static fs_type tmpfs_fs_type = { .name = "tmpfs", .get_sb = tmpfs_get_sb };
static fs_type devtmpfs_fs_type = { .name = "devtmpfs", .get_sb = stub_get_sb };
static fs_type none_fs_type = { .name = "none", .get_sb = stub_get_sb };
static fs_type devpts_fs_type = { .name = "devpts", .get_sb = devpts_get_sb };

static void mount_syscall_init(void)
{
	printk("mnt: Register mount fs types\n");
	fs_register_type(&sysfs_fs_type);
	fs_register_type(&tmpfs_fs_type);
	fs_register_type(&devtmpfs_fs_type);
	fs_register_type(&none_fs_type);
	fs_register_type(&devpts_fs_type);
}

KERNEL_INIT(2, mount_syscall_init);

/* =========================================================================
 * fs_do_mount / fs_do_umount — called by sys_mount / sys_umount
 * ====================================================================== */

int fs_do_mount(const char *dev, const char *target, const char *type,
		unsigned flags, void *data)
{
	task_struct *cur = CURRENT_TASK();
	fs_type *fst;
	super_block *sb;
	int ret;

	if (!target || *target != '/')
		return -EINVAL;

	if (!type)
		return -EINVAL;

	/* Remount of "/" — change flags on the existing root superblock. */
	if (target[1] == '\0' && (flags & MS_REMOUNT)) {
		if (!cur->root->s_op || !cur->root->s_op->remount)
			return -ENOSYS;
		return cur->root->s_op->remount(cur->root, (int)flags);
	}

	/* Non-remount mount of "/" is a no-op (already mounted at boot). */
	if (target[1] == '\0')
		return 0;

	fst = fs_find_type(type);
	if (!fst)
		return -ENODEV;

	sb = fst->get_sb(dev, target, (int)flags, data);
	if (!sb)
		return -ENOMEM;

	ret = vfs_mount(cur->root, target, sb);
	if (ret == -EEXIST) {
		sb_put(sb);
		ret = 0;
		return ret;
	}

	if (ret == 0) {
		/* Record mount metadata on the superblock for /proc/mounts.
		 * Don't overwrite s_devname if get_sb already set it
		 * (e.g. auto-looped mounts set it to "/dev/loopN"). */
		if (!sb->s_devname[0])
			strncpy(sb->s_devname, dev ? dev : type,
				sizeof(sb->s_devname) - 1);
		strncpy(sb->s_fstype, type, sizeof(sb->s_fstype) - 1);
		sb->s_flags = (int)flags;
	}

	return ret;
}

int fs_do_umount(const char *target, int flags)
{
	task_struct *cur = CURRENT_TASK();

	if (!target || *target != '/')
		return -EINVAL;

	if (target[1] == '\0')
		return -EBUSY;

	return vfs_umount(cur->root, target);
}
