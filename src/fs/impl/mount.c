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

static struct rb_root fs_types = _RBTREE_ROOT_INIT;

void fs_register_type(fs_type *fst)
{
	struct rb_node **link = &fs_types.rb_node, *parent = NULL;
	while (*link) {
		fs_type *existing = rb_entry(*link, fs_type, name_node);
		int order = strcmp(fst->name, existing->name);
		if (!order)
			return;
		parent = *link;
		link = order < 0 ? &parent->rb_left : &parent->rb_right;
	}
	rb_init_node(&fst->name_node);
	rb_link_node(&fst->name_node, parent, link);
	rb_insert_color(&fst->name_node, &fs_types);
}

static fs_type *fs_find_type(const char *name)
{
	struct rb_node *node = fs_types.rb_node;
	while (node) {
		fs_type *type = rb_entry(node, fs_type, name_node);
		int order = strcmp(name, type->name);
		if (!order)
			return type;
		node = order < 0 ? node->rb_left : node->rb_right;
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

static int stub_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;

	s->st_mode = node->i_mode;
	s->st_ino = node->i_ino;
	s->st_size = 0;
	s->st_blksize = 0;
	s->st_blocks = 0;
	return 0;
}

static const file_operations stub_fops = {
	.getattr = stub_getattr,
};

static file *stub_open_root(super_block *sb, int flag)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH |
		       S_IXOTH;
	node->i_ino = 1;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &stub_fops;
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

static fs_type devtmpfs_fs_type = { .name = "devtmpfs", .get_sb = stub_get_sb };
static fs_type none_fs_type = { .name = "none", .get_sb = stub_get_sb };

static void mount_syscall_init(void)
{
	printk("mnt: registered devtmpfs file type\n");
	fs_register_type(&devtmpfs_fs_type);

	printk("mnt: registered none file type\n");
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

	if (!target)
		return -EINVAL;

	if (!type)
		return -EINVAL;

	/* Remount of "/" — change flags on the existing root superblock. */

	if ((strcmp(target, "LABEL=/") == 0 || target[1] == '\0') &&
	    (flags & MS_REMOUNT)) {
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
		return -EBUSY;
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
