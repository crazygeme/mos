/*
 * common.c — shared helpers for /proc/net/ files.
 *
 * Provides make_text_file() which wraps a vm_alloc(1) content buffer in a
 * read-only inode/file pair.  All /proc/net/ leaf files use this.
 */
#include "proc_net.h"
#include <fs/fs.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <ext4.h>

static const file_operations plain_fops;

static ssize_t plain_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	loff_t fsize = (loff_t)fp->f_inode->i_size;
	loff_t offset = *pos;
	ssize_t left = (ssize_t)(fsize - offset);
	ssize_t n = (ssize_t)count < left ? (ssize_t)count : left;
	if (n <= 0)
		return 0;
	memcpy(buf, (char *)fp->f_inode->i_private + offset, (size_t)n);
	*pos = offset + n;
	return n;
}

static loff_t plain_llseek(file *fp, loff_t offset, int whence)
{
	loff_t fsize = (loff_t)fp->f_inode->i_size;
	loff_t newpos;
	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = fp->f_pos + offset;
		break;
	case SEEK_END:
		newpos = fsize + offset;
		break;
	default:
		return -EINVAL;
	}
	if (newpos < 0)
		return -EINVAL;
	fp->f_pos = newpos;
	return newpos;
}

static int plain_release(file *fp)
{
	vm_free((unsigned int)fp->f_inode->i_private, 1);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int plain_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;

	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = (loff_t)node->i_size;
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	return 0;
}

static const file_operations plain_fops = {
	.getattr = plain_getattr,
	.read = plain_read,
	.llseek = plain_llseek,
	.release = plain_release,
};

file *make_text_file(char *buf)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
	node->i_private = buf;
	node->i_size = strlen(buf);

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &plain_fops;
	return fp;
}
