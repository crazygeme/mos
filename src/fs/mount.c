#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <macro.h>
#include <errno.h>

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

static file *stub_open_root(super_block *sb)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH |
		       S_IXOTH;
	node->i_ino = 1;
	node->i_op = &stub_iops;

	file *fp = calloc(1, sizeof(*fp));
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

static fs_type proc_fs_type = { .name = "proc", .get_sb = stub_get_sb };
static fs_type sysfs_fs_type = { .name = "sysfs", .get_sb = stub_get_sb };
static fs_type tmpfs_fs_type = { .name = "tmpfs", .get_sb = stub_get_sb };
static fs_type devpts_fs_type = { .name = "devpts", .get_sb = stub_get_sb };
static fs_type devtmpfs_fs_type = { .name = "devtmpfs", .get_sb = stub_get_sb };
static fs_type none_fs_type = { .name = "none", .get_sb = stub_get_sb };

static void mount_syscall_init(void)
{
	fs_register_type(&proc_fs_type);
	fs_register_type(&sysfs_fs_type);
	fs_register_type(&tmpfs_fs_type);
	fs_register_type(&devpts_fs_type);
	fs_register_type(&devtmpfs_fs_type);
	fs_register_type(&none_fs_type);
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

	/*
	 * The root filesystem ("/") is set up by the kernel at boot.
	 * Userspace remount requests (e.g. "mount -o remount,rw /") are
	 * accepted silently — lwext4 is already live and there is nothing
	 * meaningful to do at the VFS level.
	 */
	if (target[1] == '\0') /* target == "/" */
		return 0;

	fst = fs_find_type(type);
	if (!fst)
		return -ENODEV;

	sb = fst->get_sb(dev, target, (int)flags, data);
	if (!sb)
		return -ENOMEM;

	ret = vfs_mount(cur->root, target, sb);
	if (ret == -EEXIST) {
		/*
		 * Path already mounted (e.g. /dev/pts set up by the kernel at
		 * boot).  Release the just-allocated super_block and report
		 * success so that init scripts are not disrupted.
		 */
		sb_put(sb);
		return 0;
	}
	return ret;
}

int fs_do_umount(const char *target, int flags)
{
	task_struct *cur = CURRENT_TASK();

	if (!target || *target != '/')
		return -EINVAL;

	/* Refuse to unmount the root filesystem. */
	if (target[1] == '\0') /* target == "/" */
		return -EBUSY;

	return vfs_umount(cur->root, target);
}
