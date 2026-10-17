#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <ps/namespace.h>
#include <lib/klib.h>
#include <macro.h>
#include <errno.h>
#include <fs/fcntl.h>

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

const fs_type *fs_probe_block(blockdev_handle *device)
{
	for (struct rb_node *n = rb_first(&fs_types); n; n = rb_next(n)) {
		fs_type *type = rb_entry(n, fs_type, name_node);
		if (type->probe && type->probe(device) > 0)
			return type;
	}
	return NULL;
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

static fs_type none_fs_type = { .name = "none", .get_sb = stub_get_sb };

static void mount_syscall_init(void)
{
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

	/* Linux mount callers may prefix flags with MS_MGC_VAL. */
	if ((flags & 0xffff0000U) == 0xc0ed0000U)
		flags &= 0xffffU;

	if (!target)
		return -EINVAL;

	if (!ps_has_capability(cur, 21))
		return -EPERM;
	if (flags & 8192) { /* MS_MOVE */
		if (!dev || (flags & ~8192U) || !strcmp(dev, target))
			return -EINVAL;
		struct stat st;
		file *destination = vfs_open(cur->fs->root, target, O_PATH);
		if (!destination)
			return -ENOENT;
		int valid = destination->f_fop && destination->f_fop->getattr &&
			    !destination->f_fop->getattr(destination, &st) &&
			    S_ISDIR(st.st_mode);
		fs_put_file(destination);
		if (!valid)
			return -ENOTDIR;
		super_block *moved = vfs_take_mount(cur->fs->root, dev);
		if (!moved)
			return -EINVAL;
		if (!strcmp(target, "/")) {
			rmutex_lock(&cur->fs->lock);
			super_block *old = cur->fs->root;
			cur->fs->root = moved;
			vfs_rebase_mount_tree(moved, "/");
			size_t length = strlen(dev);
			if (!strncmp(cur->fs->cwd, dev, length) &&
			    (!cur->fs->cwd[length] ||
			     cur->fs->cwd[length] == '/')) {
				char *suffix = strdup(cur->fs->cwd + length);
				strcpy(cur->fs->cwd,
				       suffix && *suffix ? suffix : "/");
				free(suffix);
			} else
				strcpy(cur->fs->cwd, "/");
			strcpy(cur->fs->root_path, "/");
			ps_namespace_mount_root(moved);
			rmutex_unlock(&cur->fs->lock);
			sb_put(old);
			vfs_namespace_changed();
			return 0;
		}
		int moved_result = vfs_mount(cur->fs->root, target, moved);
		if (moved_result && vfs_mount(cur->fs->root, dev, moved))
			sb_put(moved);
		return moved_result;
	}
	if (flags & ((1U << 17) | (1U << 18) | (1U << 19) | (1U << 20))) {
		ps_namespace_mount_root(cur->fs->root);
		return vfs_set_propagation(cur->fs->root, target, flags);
	}
	if (flags & MS_REMOUNT) {
		super_block *mounted = cur->fs->root;
		char *suffix;
		if (strcmp(target, "/") &&
		    (!sb_path_resolve(cur->fs->root, target, &mounted,
				      &suffix) ||
		     *suffix))
			return -EINVAL;
		if ((flags & 4096) || mounted->s_backend ||
		    !mounted->s_op->remount) {
			mounted->s_flags = flags & ~(MS_REMOUNT | 4096 | 16384);
			vfs_namespace_changed();
			return 0;
		}
		return mounted->s_op->remount(mounted, (int)flags);
	}
	if (flags & 4096) { /* MS_BIND */
		if (!dev)
			return -EINVAL;
		file *fp = vfs_open(cur->fs->root, dev, O_PATH);
		if (!fp)
			return -ENOENT;
		fs_put_file(fp);
		sb = vfs_bind_mount(cur->fs->root, dev, !!(flags & 16384));
		if (!sb)
			return -ENOMEM;
		ret = vfs_mount_propagated(cur->fs->root, target, sb);
		if (ret)
			sb_put(sb);
		return ret;
	}
	if (!type || !*type)
		return -EINVAL;

	/* Remount of "/" — change flags on the existing root superblock. */

	if ((strcmp(target, "LABEL=/") == 0 || target[1] == '\0') &&
	    (flags & MS_REMOUNT)) {
		if (!cur->fs->root->s_op || !cur->fs->root->s_op->remount)
			return -ENOSYS;
		return cur->fs->root->s_op->remount(cur->fs->root, (int)flags);
	}

	/* Non-remount mount of "/" is a no-op (already mounted at boot). */
	if (target[1] == '\0')
		return 0;

	fst = fs_find_type(type);
	if (!fst)
		return -ENODEV;

	/* Block filesystem factories require an unoccupied mount point. */
	if (!strcmp(type, "ext3") || !strcmp(type, "ext4") ||
	    !strcmp(type, "vfat")) {
		super_block *mounted;
		char *suffix;
		if (sb_path_resolve(cur->fs->root, target, &mounted, &suffix) &&
		    !*suffix)
			return -EBUSY;
	}

	sb = fst->get_sb(dev, target, (int)flags, data);
	if (!sb)
		return -ENOMEM;

	strncpy(sb->s_fstype, type, sizeof(sb->s_fstype) - 1);
	ret = vfs_mount_propagated(cur->fs->root, target, sb);
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
	if (!ps_has_capability(cur, 21))
		return -EPERM;

	if (!target || *target != '/')
		return -EINVAL;

	if (target[1] == '\0')
		return -EBUSY;

	return vfs_umount_propagated(cur->fs->root, target);
}
