#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <hw/time.h>
#include <dev/dev.h>
#include "pts_internal.h"

#define MAX_PTS 16
#define PTM_MAJOR 2 /* BSD pty master */
#define PTM_MINOR 2 /* BSD pty master */
#define PTS_MAJOR 3 /* BSD pty slave  */
#define PTS_INO_MASK 0x00050000

static pts_pair pts_pairs[MAX_PTS];
static spinlock_t pts_alloc_lock;

static int pts_master_getattr(inode *node, struct stat *s)
{
	pts_pair *p = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(PTM_MAJOR, 0);
	s->st_rdev = MKDEV(PTM_MAJOR, PTM_MINOR + p->idx);
	s->st_ino = PTS_INO_MASK | MAX_PTS;
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = PAGE_SIZE;
	s->st_size = PAGE_SIZE;
	s->st_blocks = 1;
	return 0;
}

static int pts_slave_getattr(inode *node, struct stat *s)
{
	pts_pair *p = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(PTS_MAJOR, 0);
	s->st_rdev = MKDEV(PTS_MAJOR, p->idx + 2);
	s->st_ino = (uint64_t)p->idx + 2;
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = PAGE_SIZE;
	s->st_size = PAGE_SIZE;
	s->st_blocks = 1;
	return 0;
}

/* slave */

static int pts_slave_release(file *fp)
{
	pts_pair *p = fp->f_inode->i_private;

	if (__sync_add_and_fetch(&p->slave_count, -1) == 0) {
		cyb_writer_close(p->s2m);
		cyb_reader_close(p->m2s);
		pts_pair_check_free(p, &pts_alloc_lock);
	}

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const inode_operations pts_slave_iops = {
	.getattr = pts_slave_getattr,
};

static const file_operations pts_slave_path_fops = {
	.release = pts_slave_path_release,
};

static const file_operations pts_slave_fops = {
	.release = pts_slave_release,
	.read = pts_slave_read,
	.write = pts_slave_write,
	.is_ready = pts_slave_poll,
	.poll_wait = pts_slave_poll_wait,
	.poll_wait_remove = pts_slave_poll_wait_remove,
	.ioctl = pts_slave_ioctl,
};

/* master */

static int pts_master_release(file *fp)
{
	pts_pair *p = fp->f_inode->i_private;

	cyb_writer_close(p->m2s);
	cyb_reader_close(p->s2m);

	p->master_open = 0;
	pts_pair_check_free(p, &pts_alloc_lock);

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const inode_operations pts_master_iops = {
	.getattr = pts_master_getattr,
};

static const file_operations pts_master_fops = {
	.release = pts_master_release,
	.read = pts_master_read,
	.write = pts_master_write,
	.is_ready = pts_master_poll,
	.poll_wait = pts_master_poll_wait,
	.poll_wait_remove = pts_master_poll_wait_remove,
	.ioctl = pts_master_ioctl,
};

/*
 * ptm_cdev_open — open /dev/ptypN (master).
 * Allocates and initialises pair N; fails with EBUSY if already in use.
 */
static file *ptm_cdev_open(super_block *sb, unsigned rdev, int flag)
{
	int idx = (int)MINOR(rdev);
	pts_pair *p = &pts_pairs[idx];
	int irq;

	spinlock_lock(&pts_alloc_lock, &irq);
	if (p->used) {
		spinlock_unlock(&pts_alloc_lock, irq);
		return NULL; /* -EBUSY */
	}
	memset(p, 0, sizeof(*p));
	p->idx = idx;
	p->used = 1;
	p->master_open = 1;
	p->termios = tty_default_termios;
	p->winsize.ws_row = 24;
	p->winsize.ws_col = 80;
	spinlock_init(&p->lock);
	p->m2s = cyb_create(1);
	p->s2m = cyb_create(1);
	spinlock_unlock(&pts_alloc_lock, irq);

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	node->i_op = &pts_master_iops;
	node->i_private = p;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &pts_master_fops;
	return fp;
}

static file *pts_cdev_open(super_block *sb, unsigned rdev, int flag)
{
	int idx = (int)MINOR(rdev);
	pts_pair *p = &pts_pairs[idx];
	int irq;

	spinlock_lock(&pts_alloc_lock, &irq);
	if (!p->used || !p->master_open) {
		spinlock_unlock(&pts_alloc_lock, irq);
		return NULL;
	}
	if (!(flag & O_PATH))
		__sync_add_and_fetch(&p->slave_count, 1);
	spinlock_unlock(&pts_alloc_lock, irq);

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	node->i_op = &pts_slave_iops;
	node->i_private = p;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = (flag & O_PATH) ? &pts_slave_path_fops : &pts_slave_fops;
	return fp;
}

static const char pty_hex[] = "0123456789abcdef";

static void pty_dev_register(super_block *dev_sb)
{
	int i;
	char path[16];

	cdev_register(S_IFCHR, PTM_MAJOR, 0, MAX_PTS, ptm_cdev_open);
	cdev_register(S_IFCHR, PTS_MAJOR, 0, MAX_PTS, pts_cdev_open);

	for (i = 0; i < MAX_PTS; i++) {
		sprintf(path, "/ptyp%c", pty_hex[i]);
		vfs_mknod(dev_sb, path, S_IFCHR | 0620, MKDEV(PTM_MAJOR, i));

		sprintf(path, "/ttyp%c", pty_hex[i]);
		vfs_mknod(dev_sb, path, S_IFCHR | 0620, MKDEV(PTS_MAJOR, i));
	}
}

DEV_INIT(pty_dev_register);