#include "fs/fs.h"
#include "ps/ps.h"
#include <fs/mount.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <dev/dev.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include "pts_internal.h"
#include "devnums.h"

#define MAX_PTS 16
#define PTS_INO_MASK 0x00040000

static spinlock_t pts_alloc_lock;
static pts_pair pts_pairs[MAX_PTS];

static int ptmx_dir_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFDIR | 0755;
	s->st_nlink = 2;
	s->st_ino = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = 1024;
	s->st_size = 0;
	s->st_blocks = 0;
	return 0;
}

static int ptmx_master_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(3, 1);
	s->st_rdev = MKDEV(UNIX98_PTMX_MAJOR, UNIX98_PTMX_MINOR);
	s->st_ino = PTS_INO_MASK | MAX_PTS;
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = 4096;
	s->st_size = 0;
	s->st_blocks = 0;
	return 0;
}

static int pts_slave_getattr(inode *node, struct stat *s)
{
	pts_pair *p = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = p->slave_mode;
	s->st_dev = MKDEV(0, 6);
	s->st_rdev = MKDEV(UNIX98_PTS_MAJOR, p->idx);
	s->st_ino = (uint64_t)p->idx + 2;
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_uid = p->slave_uid;
	s->st_gid = p->slave_gid;
	s->st_blksize = 1024;
	s->st_size = 0;
	s->st_blocks = 0;
	return 0;
}

/*
 * Slave
 */

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
	.release = pts_slave_path_release,
};

static const file_operations pts_slave_fops = {
	.release = pts_slave_release,
	.read = pts_slave_read,
	.write = pts_slave_write,
	.poll = pts_slave_poll,
	.ioctl = pts_slave_ioctl,
};

static const inode_operations pts_slave_iops = {
	.getattr = pts_slave_getattr,
	.setattr = pts_slave_setattr,
	.chown = pts_slave_chown,
};

file *ptmx_slave_open(super_block *sb, const char *path, int flag)
{
	int idx;
	if (!path || *path != '/')
		return NULL;

	idx = atoi(path + 1);
	if (idx < 0 || idx >= MAX_PTS)
		return NULL;

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
	node->i_mode = p->slave_mode;
	node->i_op = &pts_slave_iops;
	node->i_private = p;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = (flag & O_PATH) ? &pts_slave_path_fops : &pts_slave_fops;
	if (!(flag & O_PATH)) {
		cyb_reader_open(p->m2s);
		cyb_writer_open(p->s2m);
	}
	return fp;
}

/* Master */
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

static const file_operations ptmx_master_fops = {
	.release = pts_master_release,
	.read = pts_master_read,
	.write = pts_master_write,
	.poll = pts_master_poll,
	.ioctl = pts_master_ioctl,
};

static const inode_operations ptmx_master_iops = {
	.getattr = ptmx_master_getattr,
};

/*
 * Unix98 PTY slaves are accessed via /dev/pts/N.
 * grantpt(3) calls ptsname_r() -> TIOCGPTN to get N, then stat("/dev/pts/N")
 * to verify ownership.  We provide a minimal directory superblock mounted at
 * /dev/pts with pre-created slave device nodes /dev/pts/0 .. /dev/pts/15.
 */

static ssize_t ptmx_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	memory_dir *rd = fp->f_inode->i_private;
	loff_t offset = *pos;
	ssize_t left, read_size = 0;

	if (!rd->buf || !rd->length)
		goto done;

	left = (ssize_t)rd->length - (ssize_t)offset;
	read_size = (ssize_t)count < left ? (ssize_t)count : left;
	if (read_size > 0)
		memcpy(buf, (char *)rd->buf + offset, read_size);
	else
		read_size = 0;
done:
	*pos = offset + read_size;
	return read_size;
}

static unsigned ptmx_dir_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return (events & FS_POLL_READ) ? FS_POLL_READ : 0;
}

static int ptmx_dir_release(file *fp)
{
	memory_dir *rd = fp->f_inode->i_private;
	kfree(rd->buf);
	free(rd);
	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const inode_operations ptmx_dir_iops = {
	.getattr = ptmx_dir_getattr,
};

static const file_operations ptmx_dir_fops = {
	.release = ptmx_dir_release,
	.read = ptmx_dir_read,
	.poll = ptmx_dir_poll,
};

static int ptmx_statfs(super_block *sb, struct statfs *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->f_type = 0x1cd1; /* DEVPTS_SUPER_MAGIC */
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = 255;
	return 0;
}

static void ptmx_dir_gen(super_block *sb, memory_dir *rd)
{
	unsigned size = 0;
	int i;
	char tty_buf[12];
	char *buf, *p;
	int irq;
	const char *begin;
	struct linux_dirent *dirp;

	/* ---- Size calculation ---- */
	size += ROUND_UP(NAME_OFFSET() + 2); /* "."    strlen=1 +1 */
	size += ROUND_UP(NAME_OFFSET() + 3); /* ".."   strlen=2 +1 */

	spinlock_lock(&pts_alloc_lock, &irq);
	for (i = 0; i < MAX_PTS; i++) {
		if (pts_pairs[i].used) {
			size += ROUND_UP(
				NAME_OFFSET() +
				sprintf(tty_buf, "%d", pts_pairs[i].idx) + 1);
		}
	}
	spinlock_unlock(&pts_alloc_lock, irq);

	/* ---- Allocate and fill ---- */
	buf = p = kmalloc(size);
	begin = buf;
	memset(buf, 0, size);
	rd->buf = (struct linux_dirent *)buf;
	rd->length = size;

	FILL_ENTRY(".", 1);
	FILL_ENTRY("..", 1);

	spinlock_lock(&pts_alloc_lock, &irq);
	for (i = 0; i < MAX_PTS; i++) {
		if (pts_pairs[i].used) {
			sprintf(tty_buf, "%d", pts_pairs[i].idx);
			FILL_ENTRY(tty_buf, 1);
		}
	}
	spinlock_unlock(&pts_alloc_lock, irq);
}

static file *ptmx_dir_open_root(super_block *sb, int flag)
{
	memory_dir *rd = zalloc(sizeof(*rd));
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	ptmx_dir_gen(sb, rd);

	node->i_mode = S_IFDIR | 0755;
	node->i_op = &ptmx_dir_iops;
	node->i_private = rd;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &ptmx_dir_fops;
	return fp;
}

static void ptmx_dir_release_super(super_block *sb)
{
	kfree(sb);
}

/*
 * ptmx_cdev_open — open /dev/ptmx (Unix98).
 * Dynamically allocates any free pair; use TIOCGPTN to discover the index.
 */
static file *ptmx_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	int i;
	pts_pair *p = NULL;
	int irq;

	spinlock_lock(&pts_alloc_lock, &irq);
	for (i = 0; i < MAX_PTS; i++) {
		if (!pts_pairs[i].used) {
			p = &pts_pairs[i];
			break;
		}
	}
	if (!p) {
		spinlock_unlock(&pts_alloc_lock, irq);
		return NULL; /* -EAGAIN: no free pairs */
	}

	memset(p, 0, sizeof(*p));
	p->idx = i;
	p->used = 1;
	p->master_open = 1;
	p->pt_locked = 1;
	p->slave_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IWGRP;
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
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_op = &ptmx_master_iops;
	node->i_private = p;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &ptmx_master_fops;

	return fp;
}

static const super_operations ptmxdir_sops = {
	.open_root = ptmx_dir_open_root,
	.open = ptmx_slave_open,
	.release = ptmx_dir_release_super,
	.statfs = ptmx_statfs,
};

static super_block *ptmx_get_sb(const char *dev, const char *target, int flags,
				void *data)
{
	(void)dev;
	(void)target;
	(void)flags;
	(void)data;
	return sget(&ptmxdir_sops);
}

static fs_type devpts_fs_type = { .name = "devpts", .get_sb = ptmx_get_sb };

static void ptmx_fs_type_init(void)
{
	spinlock_init(&pts_alloc_lock);
	printk("mnt: registered devpts file type\n");
	fs_register_type(&devpts_fs_type);
}

static void ptmx_dev_register(super_block *dev_sb)
{
	printk("dev: registered /dev/ptmx\n");
	cdev_register(S_IFCHR, UNIX98_PTMX_MAJOR, UNIX98_PTMX_MINOR, 1,
		      ptmx_cdev_open);
	vfs_mknod(dev_sb, "/ptmx", S_IFCHR | 0666,
		  MKDEV(UNIX98_PTMX_MAJOR, UNIX98_PTMX_MINOR));
}

KERNEL_INIT(2, ptmx_fs_type_init);
DEV_INIT(ptmx_dev_register);
