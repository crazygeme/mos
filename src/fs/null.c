#include <time.h>
#include <null.h>
#include <unistd.h>
#include <klib.h>
#include <fs.h>
#include <mount.h>
#include <ps.h>

static ssize_t null_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	memset(buf, 0, size);
	return (ssize_t)size;
}

static ssize_t null_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	return (ssize_t)size;
}

static int null_release(inode *node, file *fp)
{
	free(node);
	return 0;
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
	s->st_mode = (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR |
		      S_IRGRP | S_IROTH);
	s->st_size = PAGE_SIZE;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	return 0;
}

static const inode_operations null_iops = {
	.getattr = null_getattr,
};

static const file_operations null_fops = {
	.release = null_release,
	.read = null_read,
	.write = null_write,
	.poll = null_poll,
};

static inode *null_get_inode(mount_point *mp)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFCHR;
	node->i_op = &null_iops;
	node->i_fop = &null_fops;
	return node;
}

static mount_op mp = {
	.get_inode = null_get_inode,
};

void null_init()
{
	task_struct *cur = CURRENT_TASK();
	do_mount(cur->root, "/dev/null", &mp);
}
