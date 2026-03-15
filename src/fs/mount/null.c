#include <fs/fs.h>
#include <fs/mount.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <unistd.h>

static ssize_t null_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	memset(buf, 0, size);
	return (ssize_t)size;
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
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_size = PAGE_SIZE;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_rdev = 0;
	return 0;
}

static const inode_operations null_iops = {
	.getattr = null_getattr,
};

static const file_operations null_fops = {
	.read = null_read,
	.write = null_write,
	.poll = null_poll,
};

static file *null_open_root(super_block *sb)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR |
			S_IRGRP | S_IROTH);
	node->i_op = &null_iops;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &null_fops;
	return fp;
}

static super_operations null_sops = {
	.open_root = null_open_root,
};

static void null_init()
{
	task_struct *cur = CURRENT_TASK();
	vfs_mount(cur->root, "/dev/null", sget(&null_sops));
}

KERNEL_INIT(4, null_init);
