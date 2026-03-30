/*
 * src/dev/devnode.c - device node created by mknod(2).
 *
 * Each successful mknod() allocates one devnode super_block and mounts it
 * into the VFS tree at the requested path.  Opening the path returns a
 * minimal file whose getattr reports the correct mode/rdev.
 *
 * S_IFIFO  : a cy_buf shared across all opens; reader/writer counts are
 *            managed by cyb_reader_open/cyb_writer_open so that EOF
 *            propagates correctly when all writers close.
 * S_IFCHR /
 * S_IFBLK /
 * S_IFSOCK : stat returns the right mode and rdev; actual I/O returns
 *            -ENXIO (no central char-device registry exists).
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <hw/time.h>
#include <dev/dev.h>
#include <errno.h>
#include <macro.h>

/* ------------------------------------------------------------------ *
 * Character / block device dispatch table                             *
 * ------------------------------------------------------------------ */

#define MAX_CDEVS 32

typedef struct {
	unsigned mode_type; /* S_IFCHR or S_IFBLK */
	unsigned major;
	unsigned minor_base;
	unsigned minor_count;
	file *(*open)(unsigned rdev, int flag);
} cdev_entry;

static cdev_entry cdev_table[MAX_CDEVS];
static int cdev_count;

void cdev_register(unsigned mode_type, unsigned major, unsigned minor_base,
		   unsigned minor_count, file *(*open)(unsigned rdev, int flag))
{
	if (cdev_count >= MAX_CDEVS)
		return;
	cdev_table[cdev_count].mode_type = mode_type;
	cdev_table[cdev_count].major = major;
	cdev_table[cdev_count].minor_base = minor_base;
	cdev_table[cdev_count].minor_count = minor_count;
	cdev_table[cdev_count].open = open;
	cdev_count++;
}

static file *cdev_dispatch(unsigned mode, unsigned rdev, int flag)
{
	unsigned mt = mode & S_IFMT;
	unsigned major = MAJOR(rdev);
	unsigned minor = MINOR(rdev);
	int i;

	for (i = 0; i < cdev_count; i++) {
		cdev_entry *e = &cdev_table[i];
		if (e->mode_type == mt && e->major == major &&
		    minor >= e->minor_base &&
		    minor < e->minor_base + e->minor_count)
			return e->open(rdev, flag);
	}
	return NULL;
}

typedef struct {
	unsigned mode; /* full mode bits, e.g. S_IFCHR | 0666 */
	unsigned rdev; /* encoded major/minor (for char/block) */
	cy_buf *fifo; /* non-NULL for S_IFIFO */
} devnode_info;

/* ------------------------------------------------------------------ *
 * Char / block / socket: stat works; I/O returns ENXIO               *
 * ------------------------------------------------------------------ */

static int devnode_getattr(inode *node, struct stat *s)
{
	devnode_info *dn = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = dn->mode;
	s->st_rdev = dn->rdev;
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = PAGE_SIZE;
	return 0;
}

static ssize_t devnode_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	return -ENXIO;
}

static ssize_t devnode_write(file *fp, const void *buf, size_t len, loff_t *pos)
{
	return -ENXIO;
}

static int devnode_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations devnode_iops = {
	.getattr = devnode_getattr,
};

static const file_operations devnode_fops = {
	.release = devnode_release,
	.read = devnode_read,
	.write = devnode_write,
};

/* ------------------------------------------------------------------ *
 * FIFO: shared cy_buf between reader and writer opens                 *
 * ------------------------------------------------------------------ */

static int fifonode_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode; /* mode was copied into i_mode at open */
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = PAGE_SIZE;
	return 0;
}

static ssize_t fifonode_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	cy_buf *b = fp->f_inode->i_private;
	return (ssize_t)cyb_getbuf(b, buf, (int)len, 1, 1);
}

static ssize_t fifonode_write(file *fp, const void *buf, size_t len,
			      loff_t *pos)
{
	cy_buf *b = fp->f_inode->i_private;
	if (cyb_reader_count(b) == 0)
		return -EPIPE;
	return (ssize_t)cyb_putbuf(b, (unsigned char *)buf, (unsigned)len, 0,
				   0);
}

static int fifonode_poll(file *fp, unsigned type)
{
	cy_buf *b = fp->f_inode->i_private;

	if (type == FS_POLL_EXCEPT)
		return -1;
	if (type == FS_POLL_READ)
		return cyb_isempty(b) ? -1 : 0;
	if (type == FS_POLL_WRITE)
		return cyb_isfull(b) ? -1 : 0;
	return -1;
}

static int fifonode_release_reader(file *fp)
{
	cy_buf *b = fp->f_inode->i_private;
	cyb_reader_close(b);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int fifonode_release_writer(file *fp)
{
	cy_buf *b = fp->f_inode->i_private;
	cyb_writer_close(b);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations fifonode_iops = {
	.getattr = fifonode_getattr,
};

static void fifonode_read_poll_wait(file *fp, task_struct *task)
{
	cy_buf *b = fp->f_inode->i_private;
	cyb_set_poll_read(b, task);
}

static void fifonode_read_poll_wait_remove(file *fp, task_struct *task)
{
	cy_buf *b = fp->f_inode->i_private;
	(void)task;
	cyb_clear_poll_read(b);
}

static void fifonode_write_poll_wait(file *fp, task_struct *task)
{
	cy_buf *b = fp->f_inode->i_private;
	cyb_set_poll_write(b, task);
}

static void fifonode_write_poll_wait_remove(file *fp, task_struct *task)
{
	cy_buf *b = fp->f_inode->i_private;
	(void)task;
	cyb_clear_poll_write(b);
}

static const file_operations fifonode_reader_fops = {
	.release = fifonode_release_reader,
	.read = fifonode_read,
	.is_ready = fifonode_poll,
	.poll_wait = fifonode_read_poll_wait,
	.poll_wait_remove = fifonode_read_poll_wait_remove,
};

static const file_operations fifonode_writer_fops = {
	.release = fifonode_release_writer,
	.write = fifonode_write,
	.is_ready = fifonode_poll,
	.poll_wait = fifonode_write_poll_wait,
	.poll_wait_remove = fifonode_write_poll_wait_remove,
};

/* ------------------------------------------------------------------ *
 * super_operations                                                     *
 * ------------------------------------------------------------------ */

static file *devnode_open_root(super_block *sb, int flag)
{
	devnode_info *dn = sb->s_fs_info;
	inode *node;
	file *fp;

	if (S_ISFIFO(dn->mode)) {
		cy_buf *b = dn->fifo;

		node = zalloc(sizeof(*node));
		node->i_mode = dn->mode;
		node->i_op = &fifonode_iops;
		node->i_private = b;

		fp = zalloc(sizeof(*fp));
		fp->f_inode = node;
		fp->f_count = 1;

		if ((flag & 3) == O_WRONLY) {
			cyb_writer_open(b);
			fp->f_fop = &fifonode_writer_fops;
		} else {
			cyb_reader_open(b);
			fp->f_fop = &fifonode_reader_fops;
		}
		return fp;
	}

	/* Char / block: dispatch to registered handler. */
	if (S_ISCHR(dn->mode) || S_ISBLK(dn->mode)) {
		file *dispatched = cdev_dispatch(dn->mode, dn->rdev, flag);
		if (dispatched)
			return dispatched;
	}

	/* No handler (or socket): stat works; I/O returns ENXIO. */
	node = zalloc(sizeof(*node));
	node->i_mode = dn->mode;
	node->i_op = &devnode_iops;
	node->i_private = dn; /* owned by super_block; not freed here */

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &devnode_fops;
	return fp;
}

static void devnode_release_super(super_block *sb)
{
	devnode_info *dn = sb->s_fs_info;
	if (dn->fifo)
		cyb_destroy(dn->fifo);
	free(dn);
	kfree(sb);
}

static const super_operations devnode_sops = {
	.open_root = devnode_open_root,
	.release = devnode_release_super,
};

/*
 * devnode_create - allocate a new devnode super_block.
 *
 * Called by vfs_mknod(); the returned super_block is mounted at the
 * requested path and thereafter owned by the VFS mount tree.
 */
super_block *devnode_create(unsigned mode, unsigned rdev)
{
	devnode_info *dn = zalloc(sizeof(*dn));
	super_block *sb;

	dn->mode = mode;
	dn->rdev = rdev;
	if (S_ISFIFO(mode))
		dn->fifo = cyb_create_named(0);

	sb = sget(&devnode_sops);
	sb->s_fs_info = dn;
	return sb;
}
