#include <time.h>
#include <timer.h>
#include <klib.h>
#include <keyboard.h>
#include <unistd.h>
#include <fs.h>
#include <tty.h>
#include <mount.h>
#include <ps.h>
#include <macro.h>

static ssize_t kb_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	char *tmp = buf;
	if (size < 1 || !buf)
		return 0;
	*tmp = kb_buf_get();
	return 1;
}

static int kb_release(inode *node, file *fp)
{
	free(node);
	return 0;
}

static int kb_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_WRITE || type == FS_POLL_EXCEPT)
		return -1;
	return kb_can_read() ? 0 : -1;
}

static int kb_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = (S_IFCHR | S_IRUSR | S_IRGRP | S_IROTH);
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0;
	s->st_rdev = 5;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_size = PAGE_SIZE;
	return 0;
}

static const inode_operations kb_iops = {
	.getattr = kb_getattr,
};

static const file_operations kb_fops = {
	.release = kb_release,
	.read = kb_read,
	.poll = kb_poll,
	.ioctl = tty_ioctl,
};

static inode *kb_get_root(super_block *sb)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFCHR;
	node->i_op = &kb_iops;
	node->i_fop = &kb_fops;
	return node;
}

static super_operations kb_sops = {
	.get_root = kb_get_root,
};

static void kbchar_init()
{
	task_struct *cur = CURRENT_TASK();
	vfs_mount(cur->root, "/dev/kb", &kb_sops);
}

KERNEL_INIT(4, kbchar_init);
