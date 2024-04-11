#include "mm.h"
#include <timer.h>
#include <debugfs.h>

static int debugfs_read(void *inode, void *buf, size_t size, size_t *rcnt)
{
	debug_inode *node = inode;
	unsigned left = 0;
	size_t read_size = 0;

	if (!node->buf || !node->len)
		goto done;

	left = node->len - node->offset;
	read_size = left > size ? size : left;
	memcpy(buf, (char *)node->buf + node->offset, read_size);
	node->offset += read_size;

done:
	if (rcnt)
		*rcnt = read_size;
	return 0;
}

static int debugfs_write(void *inode, const void *buf, size_t size,
			 size_t *wcnt)
{
	// do nothing!
	if (wcnt)
		*wcnt = size;

	return 0;
}

static int debugfs_close(void *inode)
{
	debug_inode *node = inode;
	node->len = 0;
	node->offset = 0;
	vm_free(node->buf, 1);
	node->buf = NULL;
	return 0;
}

static int debugfs_select(void *inode, unsigned type)
{
	if (type == FS_SELECT_EXCEPT)
		return -1;

	return 0;
}

static int debugfs_stat(void *inode, struct stat *s)
{
	s->st_atime = time_now();
	s->st_mode = (S_IFREG | S_IRUSR | S_IRGRP | S_IROTH);
	s->st_size = PAGE_SIZE;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now();
	s->st_dev = 0;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	return 0;
}

static fileop debugfs_op = {
	.read = debugfs_read,
	.write = debugfs_write,
	.close = debugfs_close,
	.stat = debugfs_stat,
	.select = debugfs_select,
};

filep debugfs_open(debug_inode *inode)
{
	filep fp = calloc(1, sizeof(*fp));
	inode->buf = vm_alloc(1);
	inode->fill(inode->buf, PAGE_SIZE);
	inode->len = strlen(inode->buf + 1);
	inode->offset = 0;
	fp->inode = inode;
	fp->ref_cnt = 1;
	fp->op = debugfs_op;
	fp->mode = S_IFREG;
	return fp;
}