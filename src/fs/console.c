#include <time.h>
#include <ps.h>
#include <mount.h>
#include <console.h>
#include <klib.h>
#include <unistd.h>
#include <fs.h>
#include <tty.h>

static ssize_t console_write(file *fp, const void *buf, size_t size,
			     loff_t *pos)
{
	if (size < 1 || !buf)
		return 0;
	tty_write(buf, size);
	return (ssize_t)size;
}

static ssize_t console_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	memset(buf, '?', size);
	return (ssize_t)size;
}

static int console_release(inode *node, file *fp)
{
	free(node);
	return 0;
}

static loff_t console_llseek(file *fp, loff_t offset, int whence)
{
	switch (whence) {
	case SEEK_SET:
		klib_update_cursor(offset);
		break;
	case SEEK_CUR:
		klib_update_cursor(offset + klib_get_pos());
		break;
	case SEEK_END:
		klib_update_cursor(TTY_MAX_CHARS - offset);
		break;
	default:
		break;
	}
	klib_flush_cursor();
	fp->f_pos = klib_get_pos();
	return fp->f_pos;
}

static int console_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT || type == FS_POLL_READ)
		return -1;
	/* can always write */
	return 0;
}

static int console_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR);
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0xb;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_nlink = 1;
	s->st_rdev = 8004;
	s->st_size = TTY_MAX_CHARS - klib_get_pos();
	return 0;
}

static const inode_operations console_iops = {
	.getattr = console_getattr,
};

static const file_operations console_fops = {
	.release = console_release,
	.read = console_read,
	.write = console_write,
	.llseek = console_llseek,
	.poll = console_poll,
	.ioctl = tty_ioctl,
};

static inode *console_get_inode(mount_point *mp)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFCHR;
	node->i_op = &console_iops;
	node->i_fop = &console_fops;
	return node;
}

static mount_op mp = {
	.get_inode = console_get_inode,
};

void console_init()
{
	task_struct *cur = CURRENT_TASK();
	do_mount(cur->root, "/dev/tty", &mp);
}
