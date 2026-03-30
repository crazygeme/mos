#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <dev/dev.h>
#include <unistd.h>

/* /dev/null — major 1, minor 3 (Linux-compatible) */
#define NULL_MAJOR 1
#define NULL_MINOR 3

/* /dev/zero — major 1, minor 5 (Linux-compatible) */
#define ZERO_MAJOR 1
#define ZERO_MINOR 5

static ssize_t null_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	return 0; /* EOF */
}

static ssize_t null_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	return (ssize_t)size;
}

static int null_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT)
		return -1;
	return 0;
}

static int null_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = MKDEV(NULL_MAJOR, NULL_MINOR);
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_nlink = 1;
	return 0;
}

static int null_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations null_iops = {
	.getattr = null_getattr,
};

static const file_operations null_fops = {
	.release = null_release,
	.read = null_read,
	.write = null_write,
	.is_ready = null_poll,
};

static file *null_cdev_open(unsigned rdev, int flag)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_op = &null_iops;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &null_fops;
	return fp;
}

static void null_dev_register(super_block *dev_sb)
{
	cdev_register(S_IFCHR, NULL_MAJOR, NULL_MINOR, 1, null_cdev_open);
	vfs_mknod(dev_sb, "/null", S_IFCHR | 0666,
		  MKDEV(NULL_MAJOR, NULL_MINOR));
}

DEV_INIT(null_dev_register);

/* /dev/zero */

static ssize_t zero_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	memset(buf, 0, size);
	return (ssize_t)size;
}

static ssize_t zero_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	return (ssize_t)size;
}

static int zero_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_rdev = MKDEV(ZERO_MAJOR, ZERO_MINOR);
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_nlink = 1;
	return 0;
}

static int zero_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations zero_iops = {
	.getattr = zero_getattr,
};

static const file_operations zero_fops = {
	.release = zero_release,
	.read = zero_read,
	.write = zero_write,
	.is_ready = null_poll,
};

static file *zero_cdev_open(unsigned rdev, int flag)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_op = &zero_iops;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &zero_fops;
	return fp;
}

static void zero_dev_register(super_block *dev_sb)
{
	printk("dev: registered /dev/zero\n");
	cdev_register(S_IFCHR, ZERO_MAJOR, ZERO_MINOR, 1, zero_cdev_open);
	vfs_mknod(dev_sb, "/zero", S_IFCHR | 0666,
		  MKDEV(ZERO_MAJOR, ZERO_MINOR));
}

DEV_INIT(zero_dev_register);
