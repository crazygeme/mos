#include <fs/entries.h>
#include <driver/driver.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <device/devnode.h>
#include <ps/ps.h>
#include <device/devnums.h>
#include "pts_internal.h"

#define MAX_PTS 16
#define PTS_INO_MASK 0x00050000

static pts_pair pts_pairs[MAX_PTS];
static spinlock_t pts_alloc_lock;
static struct rb_root pts_groups = _RBTREE_ROOT_INIT;

static void pty_group_insert_locked(pts_pair *p)
{
	struct rb_node **link = &pts_groups.rb_node, *parent = NULL;
	while (*link) {
		pts_pair *other = rb_entry(*link, pts_pair, group_node);
		parent = *link;
		link = p->pgrp < other->pgrp || (p->pgrp == other->pgrp &&
						 p->idx < other->idx) ?
			       &parent->rb_left :
			       &parent->rb_right;
	}
	rb_init_node(&p->group_node);
	rb_link_node(&p->group_node, parent, link);
	rb_insert_color(&p->group_node, &pts_groups);
	p->group_indexed = 1;
}

static void pty_group_changed(pts_pair *p, unsigned group)
{
	int irq;
	spinlock_lock(&pts_alloc_lock, &irq);
	if (p->group_indexed) {
		rb_erase(&p->group_node, &pts_groups);
		p->group_indexed = 0;
	}
	p->pgrp = group;
	if (p->used && p->master_open)
		pty_group_insert_locked(p);
	spinlock_unlock(&pts_alloc_lock, irq);
}

/* The pair allocator lock protects the process-group index. */
static void pty_pair_free_locked(pts_pair *p)
{
	if (p->group_indexed) {
		rb_erase(&p->group_node, &pts_groups);
		p->group_indexed = 0;
	}
}

static pts_pair *pty_group_find_locked(unsigned group)
{
	struct rb_node *node = pts_groups.rb_node;
	pts_pair *match = NULL;
	while (node) {
		pts_pair *p = rb_entry(node, pts_pair, group_node);
		if (group <= p->pgrp) {
			if (group == p->pgrp)
				match = p;
			node = node->rb_left;
		} else {
			node = node->rb_right;
		}
	}
	return match;
}

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
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
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
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
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

	pts_pair_close_master(p);
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
	pts_acquire_controlling(fp, flag);
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
	p->group_changed = pty_group_changed;
	p->on_free = pty_pair_free_locked;
	pty_group_insert_locked(p);
	p->slave_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	if (current->execution) {
		p->slave_uid = current->credentials->uid;
		p->slave_gid = current->credentials->gid;
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
	int irq;
	pts_pair *match;
	if (!task || !task->execution)
		return NULL;
	spinlock_lock(&pts_alloc_lock, &irq);
	match = pty_group_find_locked(task->thread->group_id);
	if (match && !(flag & O_PATH)) {
		__sync_add_and_fetch(&match->slave_count, 1);
		match->slave_ever_opened = 1;
	}
	spinlock_unlock(&pts_alloc_lock, irq);
	return match ? pty_open_slave_pair(match, flag) : NULL;
}

static const char pty_hex[] = "0123456789abcdef";

static void pty_dev_register(void)
{
	int i;
	char path[16];

	
	

	for (i = 0; i < MAX_PTS; i++) {
		sprintf(path, "/ptyp%c", pty_hex[i]);
		vfs_entry_device(devfs_entries(), path, S_IFCHR | 0620, MKDEV(BSD_PTM_MAJOR, i), "pty", ptm_cdev_open);

		sprintf(path, "/ttyp%c", pty_hex[i]);
		vfs_entry_device(devfs_entries(), path, S_IFCHR | 0620, MKDEV(BSD_PTS_MAJOR, i), "ttyp", pts_cdev_open);
	}
}

static int pty_dev_register_probe(void)
{
	pty_dev_register();
	return 0;
}

static driver_t pty_dev_register_driver = {
	.name = "bsd-pty",
	.bus = DEVICE_BUS_VIRTUAL,
	.virtual_id = VDEV_PTY,
	.probe_virtual = pty_dev_register_probe,
};
DRIVER_REGISTER(pty_dev_register_driver);
