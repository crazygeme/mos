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
	const char *name;
	file *(*open)(super_block *sb, unsigned rdev, int flag);
} cdev_entry;

static cdev_entry cdev_table[MAX_CDEVS];
static int cdev_count;

static const file_operations devnode_fops;
static const file_operations fifonode_reader_fops;
static const file_operations fifonode_writer_fops;

void cdev_register_named(unsigned mode_type, unsigned major,
			 unsigned minor_base, unsigned minor_count,
			 const char *name,
			 file *(*open)(super_block *sb, unsigned rdev,
				       int flag))
{
	if (cdev_count >= MAX_CDEVS)
		return;
	cdev_table[cdev_count].mode_type = mode_type;
	cdev_table[cdev_count].major = major;
	cdev_table[cdev_count].minor_base = minor_base;
	cdev_table[cdev_count].minor_count = minor_count;
	cdev_table[cdev_count].name = name;
	cdev_table[cdev_count].open = open;
	cdev_count++;
}

void cdev_for_each_major(cdev_major_iter_fn fn, void *data)
{
	int i, j, seen;

	if (!fn)
		return;

	for (i = 0; i < cdev_count; i++) {
		seen = 0;
		for (j = 0; j < i; j++) {
			if (cdev_table[j].mode_type ==
				    cdev_table[i].mode_type &&
			    cdev_table[j].major == cdev_table[i].major) {
				seen = 1;
				break;
			}
		}
		if (!seen)
			fn(cdev_table[i].mode_type, cdev_table[i].major,
			   cdev_table[i].name, data);
	}
}

static file *devnode_open_stub(super_block *sb, unsigned mode)
{
	file *fp;
	inode *node;

	node = zalloc(sizeof(*node));
	node->i_mode = mode;
	node->i_private =
		sb->s_fs_info; /* owned by super_block; not freed here */

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &devnode_fops;
	return fp;
}

typedef struct {
	unsigned mode; /* full mode bits, e.g. S_IFCHR | 0666 */
	unsigned rdev; /* encoded major/minor (for char/block) */
	cy_buf *fifo; /* non-NULL for S_IFIFO */
} devnode_info;

/* ------------------------------------------------------------------ *
 * Char / block / socket: stat works; I/O returns ENXIO               *
 * ------------------------------------------------------------------ */

static int devnode_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
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

static const file_operations devnode_fops = {
	.release = devnode_release,
	.getattr = devnode_getattr,
	.read = devnode_read,
	.write = devnode_write,
};

/* ------------------------------------------------------------------ *
 * FIFO: shared cy_buf between reader and writer opens                 *
 * ------------------------------------------------------------------ */

static int fifonode_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
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

static unsigned fifonode_poll_common(cy_buf *b, unsigned events, poll_table *pt)
{
	unsigned ready = 0;

	if ((events & FS_POLL_READ) && !cyb_isempty(b))
		ready |= FS_POLL_READ;
	if ((events & FS_POLL_WRITE) && !cyb_isfull(b))
		ready |= FS_POLL_WRITE;
	if (!ready && pt) {
		if (events & FS_POLL_READ)
			cyb_poll_read(b, pt);
		if (events & FS_POLL_WRITE)
			cyb_poll_write(b, pt);
	}
	return ready;
}

static unsigned fifonode_read_poll(file *fp, unsigned events, poll_table *pt)
{
	cy_buf *b = fp->f_inode->i_private;
	return fifonode_poll_common(b, events & FS_POLL_READ, pt);
}

static unsigned fifonode_write_poll(file *fp, unsigned events, poll_table *pt)
{
	cy_buf *b = fp->f_inode->i_private;
	return fifonode_poll_common(b, events & FS_POLL_WRITE, pt);
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

static const file_operations fifonode_reader_fops = {
	.release = fifonode_release_reader,
	.getattr = fifonode_getattr,
	.read = fifonode_read,
	.poll = fifonode_read_poll,
};

static const file_operations fifonode_writer_fops = {
	.release = fifonode_release_writer,
	.getattr = fifonode_getattr,
	.write = fifonode_write,
	.poll = fifonode_write_poll,
};

static file *devnode_open_fifo(devnode_info *dn, int flag)
{
	cy_buf *b = dn->fifo;
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	node->i_mode = dn->mode;
	node->i_private = b;

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

static file *devnode_open_node(super_block *sb, devnode_info *dn, int flag)
{
	file *fp = NULL;
	unsigned mt = dn->mode & S_IFMT;
	unsigned major = MAJOR(dn->rdev);
	unsigned minor = MINOR(dn->rdev);
	int i;

	for (i = 0; i < cdev_count; i++) {
		cdev_entry *e = &cdev_table[i];
		if (e->open && e->mode_type == mt && e->major == major &&
		    minor >= e->minor_base &&
		    minor < e->minor_base + e->minor_count) {
			fp = e->open(sb, dn->rdev, flag);
			break;
		}
	}

	if (fp)
		return fp;

	return devnode_open_stub(sb, dn->mode);
}

/* ------------------------------------------------------------------ *
 * super_operations                                                     *
 * ------------------------------------------------------------------ */

static file *devnode_open_root(super_block *sb, int flag)
{
	devnode_info *dn = sb->s_fs_info;

	if (S_ISFIFO(dn->mode))
		return devnode_open_fifo(dn, flag);
	else if (S_ISCHR(dn->mode) || S_ISBLK(dn->mode))
		return devnode_open_node(sb, dn, flag);

	return devnode_open_stub(sb, dn->mode);
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
