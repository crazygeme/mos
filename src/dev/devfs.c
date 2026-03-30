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
#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <hw/time.h>
#include <ps/ps.h>
#include <dev/dev.h>
#include <macro.h>
#include <ext4.h>

/* ------------------------------------------------------------------ *
 * Root-directory private state (one per open(2) call on /dev)         *
 * ------------------------------------------------------------------ */

typedef struct {
	struct linux_dirent *buf;
	unsigned length;
} dev_root_dir;

/* ------------------------------------------------------------------ *
 * inode/file operations for the /dev root directory                   *
 * ------------------------------------------------------------------ */

static ssize_t dev_root_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	dev_root_dir *rd = fp->f_inode->i_private;
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

static int dev_root_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT || type == FS_POLL_WRITE)
		return -1;
	return 0;
}

static int dev_root_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_atime = time_unix_sec();
	s->st_mtime = time_unix_sec();
	s->st_ctime = time_unix_sec();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_dev = 0xc;
	s->st_nlink = 2;
	s->st_ino = DEV_INODE;
	return 0;
}

static int dev_root_release(file *fp)
{
	dev_root_dir *rd = fp->f_inode->i_private;
	kfree(rd->buf);
	free(rd);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations dev_root_iops = {
	.getattr = dev_root_getattr,
};

static const file_operations dev_root_fops = {
	.read = dev_root_read,
	.is_ready = dev_root_poll,
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
static void dev_dir_gen(super_block *sb, dev_root_dir *rd)
{
	key_value_pair *kv;
	unsigned size = 0;
	char *buf, *p;
	const char *begin;
	struct linux_dirent *dirp;

	/* ---- Size calculation ---- */
	size += ROUND_UP(NAME_OFFSET() + 2); /* "."  strlen=1 +1 */
	size += ROUND_UP(NAME_OFFSET() + 3); /* ".." strlen=2 +1 */

	mutex_lock(&sb->s_lock);
	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv)) {
		/* key is "/name"; display "name" (key+1) */
		size += ROUND_UP(NAME_OFFSET() + strlen((char *)kv->key + 1) +
				 1);
	}
	mutex_unlock(&sb->s_lock);

	/* ---- Allocate and fill ---- */
	buf = p = kmalloc(size);
	begin = buf;
	memset(buf, 0, size);
	rd->buf = (struct linux_dirent *)buf;
	rd->length = size;

#define FILL_ENTRY(name_str)                                               \
	do {                                                               \
		dirp = (struct linux_dirent *)p;                           \
		dirp->d_ino = DEV_INODE;                                   \
		strcpy(dirp->d_name, (name_str));                          \
		dirp->d_reclen =                                           \
			ROUND_UP(NAME_OFFSET() + strlen(name_str) + 1);    \
		dirp->d_off = (unsigned long)(p + dirp->d_reclen - begin); \
		p += dirp->d_reclen;                                       \
	} while (0)

	FILL_ENTRY(".");
	FILL_ENTRY("..");

	mutex_lock(&sb->s_lock);
	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv))
		FILL_ENTRY((char *)kv->key + 1);
	mutex_unlock(&sb->s_lock);

#undef FILL_ENTRY
}

/* ------------------------------------------------------------------ *
 * super_operations for /dev                                            *
 * ------------------------------------------------------------------ */

static file *dev_open_root(super_block *sb, int flag)
{
	dev_root_dir *rd = zalloc(sizeof(*rd));
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	dev_dir_gen(sb, rd);

	node->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR |
		       S_IXGRP | S_IXOTH;
	node->i_op = &dev_root_iops;
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

static int dev_statfs(super_block *sb, struct statfs *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->f_type = 0x1373; /* DEVFS_SUPER_MAGIC */
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = 255;
	return 0;
}

static super_operations dev_sops = {
	.open_root = dev_open_root,
	.open = dev_open,
	.release = dev_release_super,
	.statfs = dev_statfs,
};

/* ------------------------------------------------------------------ *
 * Initialisation                                                       *
 * ------------------------------------------------------------------ */

static void devfs_init(void)
{
	task_struct *cur = CURRENT_TASK();
	dev_init_fn_t *fn;
	super_block *sb = sget(&dev_sops);

	printk("mnt: Mounting devfs on /dev\n");

	vfs_mount(cur->root, "/dev", sb);

	/* Let each device self-register under the devfs superblock. */
	for (fn = __devfs_init_start; fn < __devfs_init_end; fn++)
		(*fn)(sb);
}

KERNEL_INIT(6, devfs_init);
