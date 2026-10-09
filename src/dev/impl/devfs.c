/*
 * src/dev/devfs.c — /dev filesystem.
 *
 * Mounts at /dev and provides a directory listing of all registered device
 * nodes.  Each device self-registers by placing a pointer in the ".devfs_init"
 * ELF section (DEV_INIT macro).  devfs_init() iterates that section and calls
 * each function with the devfs superblock, which mounts the device as a child
 * superblock at its chosen path.
 *
 * The root directory listing is generated on demand from the child mount table
 * (sb->s_mounts), showing only the entries that actually live under /dev.
 */

#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <device/time.h>
#include <ps/ps.h>
#include <dev/dev.h>
#include <macro.h>
#include <ext4.h>
#include <proc/proc.h>
#include <errno.h>

/* ------------------------------------------------------------------ *
 * Root-directory private state (one per open(2) call on /dev)         *
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * inode/file operations for the /dev root directory                   *
 * ------------------------------------------------------------------ */

static ssize_t dev_root_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	memory_dir *rd = fp->f_inode->i_private;
	loff_t offset = *pos;
	ssize_t left, read_size = 0;

	if (!rd->buf || !rd->length)
		goto done;

	left = (ssize_t)rd->length - (ssize_t)offset;
	read_size = (ssize_t)count < left ? (ssize_t)count : left;
	if (read_size > 0)
		memcpy(buf, (char *)rd->buf + offset, read_size);
	else
		read_size = 0;
done:
	*pos = offset + read_size;
	return read_size;
}

static unsigned dev_root_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return (events & FS_POLL_READ) ? FS_POLL_READ : 0;
}

static int dev_root_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	memory_dir *rd = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_atime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = rd->node_count;
	s->st_size = rd->total_size;
	s->st_dev = 0xc;
	s->st_nlink = 2;
	s->st_ino = DEV_INODE;
	return 0;
}

static int dev_root_release(file *fp)
{
	memory_dir *rd = fp->f_inode->i_private;
	kfree(rd->buf);
	free(rd);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations dev_root_fops = {
	.getattr = dev_root_getattr,
	.read = dev_root_read,
	.poll = dev_root_poll,
	.release = dev_root_release,
};

/* ------------------------------------------------------------------ *
 * dev_dir_gen — build the packed linux_dirent buffer for /dev         *
 * ------------------------------------------------------------------ */

/*
 * Generate entries: "."  ".."  <child mounts>
 *
 * The child mounts come from sb->s_mounts whose keys are paths like "/tty";
 * we strip the leading '/' when emitting the dirent name.
 */
static void dev_dir_gen(super_block *sb, memory_dir *rd)
{
	struct rb_node *node;
	unsigned size = 0;
	char *buf, *p;
	const char *begin;
	struct linux_dirent *dirp;

	rd->node_count = 0;
	/* ---- Size calculation ---- */
	size += ROUND_UP(NAME_OFFSET() + 2); /* "."  strlen=1 +1 */
	size += ROUND_UP(NAME_OFFSET() + 3); /* ".." strlen=2 +1 */

	mutex_lock(&sb->s_lock);
	for (node = rb_first(&sb->s_mounts); node; node = rb_next(node)) {
		vfs_mount_node *mount = rb_entry(node, vfs_mount_node, rb_node);
		/* key is "/name"; display "name" (key+1) */
		size += ROUND_UP(NAME_OFFSET() + strlen(mount->path + 1) + 1);
		rd->node_count++;
		rd->total_size += 1024;
	}
	mutex_unlock(&sb->s_lock);

	/* ---- Allocate and fill ---- */
	buf = p = kmalloc(size);
	begin = buf;
	memset(buf, 0, size);
	rd->buf = (struct linux_dirent *)buf;
	rd->length = size;

	FILL_ENTRY(".", DEV_INODE);
	FILL_ENTRY("..", DEV_INODE);

	mutex_lock(&sb->s_lock);
	for (node = rb_first(&sb->s_mounts); node; node = rb_next(node)) {
		vfs_mount_node *mount = rb_entry(node, vfs_mount_node, rb_node);
		FILL_ENTRY(mount->path + 1, DEV_INODE);
	}
	mutex_unlock(&sb->s_lock);
}

/* ------------------------------------------------------------------ *
 * super_operations for /dev                                            *
 * ------------------------------------------------------------------ */

static file *dev_open_root(super_block *sb, int flag)
{
	memory_dir *rd = zalloc(sizeof(*rd));
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	dev_dir_gen(sb, rd);

	node->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR |
		       S_IXGRP | S_IXOTH;
	if (sb->s_fs_info)
		node->i_mode = (unsigned)(uintptr_t)sb->s_fs_info;
	node->i_private = rd;

	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &dev_root_fops;
	return fp;
}

static void dev_release_super(super_block *sb)
{
	free(sb);
}

static file *dev_open(super_block *sb, const char *path, int flag)
{
	// Everything should be added by mount
	return NULL;
}

static int dev_statfs(super_block *sb, struct statfs64 *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->f_type = 0x1373; /* DEVFS_SUPER_MAGIC */
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = 255;
	return 0;
}

static super_operations dev_sops;

/* Each directory owns the child mounts enumerated by its open operation. */
static int dev_mkdir(super_block *sb, const char *path, unsigned mode)
{
	super_block *directory;
	file *existing;
	char *name;
	size_t length;
	int result;
	struct stat parent = { .st_mode = S_IFDIR | 0555 };
	if (!path || path[0] != '/')
		return -EINVAL;
	if (sb->s_fs_info)
		parent.st_mode = (unsigned)(uintptr_t)sb->s_fs_info;
	result = fs_check_perm(&parent, W_OK | X_OK);
	if (result)
		return result;
	length = strlen(path);
	while (length > 1 && path[length - 1] == '/')
		length--;
	if (length == 1)
		return -EEXIST;
	/* VFS resolves existing parent directories before dispatching mkdir. */
	for (size_t index = 1; index < length; index++)
		if (path[index] == '/')
			return -ENOENT;
	existing = vfs_open(sb, path, O_PATH | O_NOFOLLOW);
	if (existing) {
		fs_put_file(existing);
		return -EEXIST;
	}
	name = malloc(length + 1);
	directory = sget(&dev_sops);
	if (!name || !directory) {
		free(name);
		if (directory)
			sb_put(directory);
		return -ENOMEM;
	}
	memcpy(name, path, length);
	name[length] = 0;
	directory->s_fs_info = (void *)(uintptr_t)(S_IFDIR | (mode & 0777));
	result = vfs_mount(sb, name, directory);
	free(name);
	if (result)
		sb_put(directory);
	return result;
}

static super_operations dev_sops = {
	.open_root = dev_open_root,
	.open = dev_open,
	.release = dev_release_super,
	.statfs = dev_statfs,
	.mkdir = dev_mkdir,
};

/* ------------------------------------------------------------------ *
 * Initialisation                                                       *
 * ------------------------------------------------------------------ */

static super_block *devfs_sb;

/* /dev/fd is a descriptor directory for the calling process. */
static file *dev_fd_open_root(super_block *sb, int flag)
{
	return proc_pid_lookup(CURRENT_TASK()->life->psid, "/fd", flag);
}

static file *dev_fd_open(super_block *sb, const char *path, int flag)
{
	char *name = name_get();
	file *fp;
	if (!name || strlen(path) + 4 >= MAX_PATH) {
		if (name)
			name_put(name);
		return NULL;
	}
	sprintf(name, "/fd%s", path);
	fp = proc_pid_lookup(CURRENT_TASK()->life->psid, name, flag);
	name_put(name);
	return fp;
}

static int dev_fd_readlink(super_block *sb, const char *path, char *buf,
			   size_t bufsiz, size_t *rcnt)
{
	char *name = name_get();
	int ret;
	if (!name || strlen(path) + sizeof("/proc/self/fd") > MAX_PATH) {
		if (name)
			name_put(name);
		return -ENOENT;
	}
	sprintf(name, "/proc/self/fd%s", path);
	ret = vfs_readlink(CURRENT_TASK()->fs->root, name, buf, bufsiz, rcnt);
	name_put(name);
	return ret;
}

static const super_operations dev_fd_sops = {
	.open_root = dev_fd_open_root,
	.open = dev_fd_open,
	.readlink = dev_fd_readlink,
	.release = dev_release_super,
};

void dev_node_add(const char *name, unsigned mode, unsigned devno)
{
	char path[32];
	if (!devfs_sb)
		return;
	sprintf(path, "/%s", name);
	vfs_mknod(devfs_sb, path, mode, devno);
}

void dev_node_remove(const char *name)
{
	char path[32];
	if (!devfs_sb)
		return;
	sprintf(path, "/%s", name);
	vfs_umount(devfs_sb, path);
}

static void devfs_init(void)
{
	task_struct *cur = CURRENT_TASK();
	dev_init_fn_t *fn;
	super_block *sb = sget(&dev_sops);

	/* Set mount metadata before mounting so vfs_mount_walk can emit it. */
	strncpy(sb->s_devname, "devtmpfs", sizeof(sb->s_devname) - 1);
	strncpy(sb->s_fstype, "devtmpfs", sizeof(sb->s_fstype) - 1);

	devfs_sb = sb;
	printk("mnt: Mounting devfs on /dev\n");
	vfs_mount(cur->fs->root, "/dev", sb);
	vfs_mount(sb, "/fd", sget(&dev_fd_sops));

	/* Let each device self-register under the devfs superblock. */
	for (fn = __devfs_init_start; fn < __devfs_init_end; fn++)
		(*fn)(sb);
}

KERNEL_INIT(6, devfs_init);
