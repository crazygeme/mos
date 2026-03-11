#include <mm/mm.h>
#include <hw/time.h>
#include <fs/super/debugfs.h>
#include <ext4.h>

static ssize_t debugfs_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	debug_inode *node = fp->f_inode->i_private;
	size_t left, read_size = 0;

	if (!node->buf || !node->len)
		goto done;

	left = node->len - node->offset;
	read_size = left > size ? size : left;
	memcpy(buf, (char *)node->buf + node->offset, read_size);
	node->offset += read_size;

done:
	*pos += read_size;
	return (ssize_t)read_size;
}

static ssize_t debugfs_write(file *fp, const void *buf, size_t size,
			     loff_t *pos)
{
	return (ssize_t)size;
}

static int debugfs_release(inode *node, file *fp)
{
	debug_inode *di = node->i_private;
	di->len = 0;
	di->offset = 0;
	vm_free(di->buf, 1);
	di->buf = NULL;
	/* node wraps a hash-table-owned debug_inode; only free the wrapper */
	free(node);
	return 0;
}

static int debugfs_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT)
		return -1;
	return 0;
}

static int debugfs_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = (S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
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

static const inode_operations debugfs_iops = {
	.getattr = debugfs_getattr,
};

static const file_operations debugfs_fops = {
	.release = debugfs_release,
	.read = debugfs_read,
	.write = debugfs_write,
	.poll = debugfs_poll,
};

file *debugfs_open(debug_inode *di)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFREG;
	node->i_op = &debugfs_iops;
	node->i_fop = &debugfs_fops;
	node->i_private = di;

	di->buf = vm_alloc(1);
	di->fill(di->buf, PAGE_SIZE);
	di->len = strlen(di->buf);
	di->offset = 0;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_op = &debugfs_fops;
	fp->f_count = 1;
	fp->f_mode = S_IFREG;
	return fp;
}
