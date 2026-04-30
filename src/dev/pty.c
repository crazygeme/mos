#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <hw/time.h>
#include <dev/dev.h>
#include <ps/ps.h>
#include "devnums.h"
#include "pts_internal.h"

#define MAX_PTS 16
#define PTS_INO_MASK 0x00050000

static pts_pair pts_pairs[MAX_PTS];
static spinlock_t pts_alloc_lock;

static int pts_master_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	pts_pair *p = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(BSD_PTM_MAJOR, 0);
	s->st_rdev = MKDEV(BSD_PTM_MAJOR, BSD_PTM_MINOR + p->idx);
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

static int pts_slave_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	pts_pair *p = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = p->slave_mode;
	s->st_dev = MKDEV(BSD_PTS_MAJOR, 0);
	s->st_rdev = MKDEV(BSD_PTS_MAJOR, p->idx + 2);
	s->st_ino = (uint64_t)p->idx + 2;
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_uid = p->slave_uid;
	s->st_gid = p->slave_gid;
	s->st_blksize = PAGE_SIZE;
	s->st_size = PAGE_SIZE;
	s->st_blocks = 1;
	return 0;
}

/* slave */

static int pts_slave_release(file *fp)
{
	pts_pair *p = fp->f_inode->i_private;

	cyb_writer_close(p->s2m);
	cyb_reader_close(p->m2s);
	if (__sync_add_and_fetch(&p->slave_count, -1) == 0) {
		pts_pair_check_free(p, &pts_alloc_lock);
	}

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const file_operations pts_slave_path_fops = {
	.getattr = pts_slave_getattr,
	.setattr = pts_slave_setattr,
	.chown = pts_slave_chown,
	.release = pts_slave_path_release,
};

static const file_operations pts_slave_fops = {
	.release = pts_slave_release,
	.getattr = pts_slave_getattr,
	.setattr = pts_slave_setattr,
	.chown = pts_slave_chown,
	.read = pts_slave_read,
	.write = pts_slave_write,
	.poll = pts_slave_poll,
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

static const file_operations pts_master_fops = {
	.release = pts_master_release,
	.getattr = pts_master_getattr,
	.read = pts_master_read,
	.write = pts_master_write,
	.poll = pts_master_poll,
	.ioctl = pts_master_ioctl,
};

static file *pty_open_slave_pair(pts_pair *p, int flag)
{
	inode *node;
	file *fp;

	node = zalloc(sizeof(*node));
	node->i_mode = p->slave_mode;
	node->i_private = p;

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = (flag & O_PATH) ? &pts_slave_path_fops : &pts_slave_fops;
	if (!(flag & O_PATH)) {
		cyb_reader_open(p->m2s);
		cyb_writer_open(p->s2m);
	}
	return fp;
}

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
	p->slave_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	if (current->user) {
		p->slave_uid = current->user->uid;
		p->slave_gid = current->user->gid;
	}
	p->termios = tty_default_termios;
	p->winsize.ws_row = 24;
	p->winsize.ws_col = 80;
	spinlock_init(&p->lock);
	p->m2s = cyb_create_named(1);
	p->s2m = cyb_create_named(1);
	spinlock_unlock(&pts_alloc_lock, irq);
	cyb_writer_open(p->m2s);
	cyb_reader_open(p->s2m);

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
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
	if (!(flag & O_PATH)) {
		__sync_add_and_fetch(&p->slave_count, 1);
		p->slave_ever_opened = 1;
	}
	spinlock_unlock(&pts_alloc_lock, irq);

	return pty_open_slave_pair(p, flag);
}

file *pty_open_controlling(task_struct *task, int flag)
{
	int i;
	int irq;
	pts_pair *match = NULL;

	if (!task || !task->user)
		return NULL;

	spinlock_lock(&pts_alloc_lock, &irq);
	for (i = 0; i < MAX_PTS; i++) {
		pts_pair *p = &pts_pairs[i];

		if (!p->used || !p->master_open)
			continue;
		if (p->pgrp != task->user->group_id)
			continue;
		if (!(flag & O_PATH)) {
			__sync_add_and_fetch(&p->slave_count, 1);
			p->slave_ever_opened = 1;
		}
		match = p;
		break;
	}
	spinlock_unlock(&pts_alloc_lock, irq);

	return match ? pty_open_slave_pair(match, flag) : NULL;
}

static const char pty_hex[] = "0123456789abcdef";

static void pty_dev_register(super_block *dev_sb)
{
	int i;
	char path[16];

	cdev_register_named(S_IFCHR, BSD_PTM_MAJOR, 0, MAX_PTS, "pty",
			    ptm_cdev_open);
	cdev_register_named(S_IFCHR, BSD_PTS_MAJOR, 0, MAX_PTS, "ttyp",
			    pts_cdev_open);

	for (i = 0; i < MAX_PTS; i++) {
		sprintf(path, "/ptyp%c", pty_hex[i]);
		vfs_mknod(dev_sb, path, S_IFCHR | 0620,
			  MKDEV(BSD_PTM_MAJOR, i));

		sprintf(path, "/ttyp%c", pty_hex[i]);
		vfs_mknod(dev_sb, path, S_IFCHR | 0620,
			  MKDEV(BSD_PTS_MAJOR, i));
	}
}

DEV_INIT(pty_dev_register);
