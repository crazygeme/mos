/*
 * src/fs/mount/initctl.c - /dev/initctl named pipe
 *
 * A single persistent cyclebuf shared across all opens of /dev/initctl.
 * Writers push init requests; the init process reads them.
 * Semantics match a FIFO: S_IFIFO mode, no seeking.
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <ps/ps.h>
#include <hw/time.h>
#include <macro.h>
#include <dev/dev.h>
#include <unistd.h>

static ssize_t initctl_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	cy_buf *initctl_buf = fp->f_inode->i_private;

	return cyb_getbuf(initctl_buf, (unsigned char *)buf, (unsigned)len, 1);
}

static ssize_t initctl_write(file *fp, const void *buf, size_t len, loff_t *pos)
{
	cy_buf *initctl_buf = fp->f_inode->i_private;

	return cyb_putbuf(initctl_buf, (unsigned char *)buf, (unsigned)len);
}

static int initctl_poll(file *fp, unsigned type)
{
	cy_buf *initctl_buf = fp->f_inode->i_private;
	if (type == FS_POLL_EXCEPT)
		return -1;
	if (type == FS_POLL_READ)
		return cyb_isempty(initctl_buf) ? -1 : 0;
	if (type == FS_POLL_WRITE)
		return cyb_isfull(initctl_buf) ? -1 : 0;
	return -1;
}

static loff_t initctl_llseek(file *fp, loff_t offset, int whence)
{
	/* named pipes are not seekable */
	return 0;
}

static int initctl_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_size = 0;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0;
	s->st_rdev = 0;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	return 0;
}

static const inode_operations initctl_iops = {
	.getattr = initctl_getattr,
};

static const file_operations initctl_fops = {
	.read = initctl_read,
	.write = initctl_write,
	.poll = initctl_poll,
	.llseek = initctl_llseek,
};

static file *initctl_open_root(super_block *sb)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	node->i_op = &initctl_iops;
	node->i_private = sb->s_fs_info;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &initctl_fops;
	return fp;
}

static void initctl_release_super(super_block *sb)
{
	cy_buf *initctl_buf = sb->s_fs_info;
	cyb_destroy(initctl_buf);
	free(sb);
}

static super_operations initctl_sops = {
	.open_root = initctl_open_root,
	.release = initctl_release_super,
};

static void initctl_dev_register(super_block *dev_sb)
{
	super_block *sb = sget(&initctl_sops);
	sb->s_fs_info = cyb_create();
	vfs_mount(dev_sb, "/initctl", sb);
}

DEV_INIT(initctl_dev_register);
