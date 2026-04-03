/*
 * src/dev/pts.c - BSD pseudo-terminal pairs (RH9 / Linux 2.4 style)
 *
 * Implements the classic BSD PTY naming used on RedHat 9 / Linux 2.4:
 *   /dev/ptyp0 … /dev/ptypf  — PTY masters  (major 2, minor 0-15)
 *   /dev/ttyp0 … /dev/ttypf  — PTY slaves   (major 3, minor 0-15)
 *
 * Usage:
 *   fd  = open("/dev/ptyp0", O_RDWR)  -- grab master of pair 0
 *         → fails with EBUSY if another process already owns it
 *   sfd = open("/dev/ttyp0", O_RDWR)  -- open slave of pair 0
 *
 * PTY pipe mechanics:
 *   m2s: master writes → slave reads  (with slave line discipline)
 *   s2m: slave writes  → master reads (raw)
 */

#include "config.h"
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/vfs.h>
#include <fs/ioctl.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <hw/time.h>
#include <ps/ps.h>
#include <macro.h>
#include <dev/dev.h>
#include <errno.h>
#include <unistd.h>
#include "tty_ldisc.h"

/* ── Configuration ────────────────────────────────────────────────────────── */

#define MAX_PTS 16
#define PTM_MAJOR 2 /* BSD pty master */
#define PTS_MAJOR 3 /* BSD pty slave  */
#define PTMX_MAJOR 5 /* Unix98 /dev/ptmx */
#define PTMX_MINOR 2

/* Unix98 ioctls */
#define TIOCGPTN 0x80045430 /* get pty number */
#define TIOCSPTLCK 0x40045431 /* lock/unlock slave */
#define TIOCGPTLCK 0x80045439 /* query slave lock state */
#define PTS_INO_MASK 0x00040000

/* ── Per-PTY state ────────────────────────────────────────────────────────── */

typedef struct {
	int idx;
	int used;
	spinlock_t lock;

	struct termios termios;
	struct winsize winsize;
	unsigned pgrp;

	/*
	 * m2s: master is writer, slave is reader
	 * s2m: slave is writer, master is reader
	 */
	cy_buf *m2s;
	cy_buf *s2m;

	tty_canon_t canon;

	int master_open;
	int slave_count;
} pts_pair;

static pts_pair pts_pairs[MAX_PTS];
static spinlock_t pts_alloc_lock;
static super_block *ptsdir_sb_global;

/* ── Pair lifecycle ───────────────────────────────────────────────────────── */

static void pts_pair_check_free(pts_pair *p)
{
	cy_buf *m2s = NULL, *s2m = NULL;
	int irq;

	spinlock_lock(&pts_alloc_lock, &irq);
	if (!p->master_open && p->slave_count == 0) {
		p->used = 0;
		/* Drop the slave-side refs that were pre-allocated by cyb_create
		 * but never consumed (no slave ever opened this pair). */
		m2s = p->m2s;
		s2m = p->s2m;
		p->m2s = p->s2m = NULL;
	}
	spinlock_unlock(&pts_alloc_lock, irq);

	if (m2s) {
		cyb_reader_close(m2s);
		cyb_writer_close(s2m);
	}
}

/* ── Slave line discipline helpers ────────────────────────────────────────── */

static int pts_canon_readline(pts_pair *p)
{
	return tty_ldisc_canon_readline(&p->canon, &p->termios, p->m2s, 1,
					p->pgrp, NULL, NULL);
}

/* ── Slave file operations ────────────────────────────────────────────────── */

static ssize_t pts_slave_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	if (!buf || size < 1)
		return 0;
	if (p->termios.c_lflag & ICANON) {
		if (p->canon.len == 0 && !pts_canon_readline(p))
			return 0;
		return (ssize_t)tty_canon_drain(&p->canon, (char *)buf,
						(int)size);
	}
	return (ssize_t)cyb_getbuf(p->m2s, buf, (int)size, 1, 1);
}

static ssize_t pts_slave_write(file *fp, const void *buf, size_t size,
			       loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	const unsigned char *src = (const unsigned char *)buf;
	if (!buf || size < 1)
		return 0;
	if (cyb_reader_count(p->s2m) == 0)
		return -EIO;
	if ((p->termios.c_oflag & OPOST) && (p->termios.c_oflag & ONLCR)) {
		/* Translate \n → \r\n: write spans between newlines in bulk */
		unsigned char crnl[2] = { '\r', '\n' };
		unsigned i = 0;
		while (i < (unsigned)size) {
			unsigned j = i;
			while (j < (unsigned)size && src[j] != '\n')
				j++;
			if (j > i)
				cyb_putbuf(p->s2m, (unsigned char *)src + i,
					   j - i, 0, 0);
			if (j < (unsigned)size) {
				cyb_putbuf(p->s2m, crnl, 2, 0, 0);
				j++;
			}
			i = j;
		}
	} else {
		cyb_putbuf(p->s2m, (unsigned char *)src, (unsigned)size, 0, 0);
	}
	return (ssize_t)size;
}

static int pts_slave_poll(file *fp, unsigned type)
{
	pts_pair *p = fp->f_inode->i_private;
	if (type == FS_POLL_READ)
		return cyb_isempty(p->m2s) ? -1 : 0;
	if (type == FS_POLL_WRITE)
		return 0;
	return -1;
}

static int pts_slave_ioctl(file *fp, unsigned cmd, void *buf)
{
	pts_pair *p = fp->f_inode->i_private;
	switch (cmd) {
	case TCGETS:
		memcpy(buf, &p->termios, sizeof(p->termios));
		return 0;
	case TCSETS:
	case TCSETSW:
		memcpy(&p->termios, buf, sizeof(p->termios));
		return 0;
	case TCSETSF:
		p->canon.len = 0;
		cyb_flush(p->m2s);
		memcpy(&p->termios, buf, sizeof(p->termios));
		return 0;
	case TIOCGWINSZ:
		memcpy(buf, &p->winsize, sizeof(p->winsize));
		return 0;
	case TIOCSWINSZ:
		memcpy(&p->winsize, buf, sizeof(p->winsize));
		return 0;
	case TIOCGPGRP:
		*(unsigned *)buf = p->pgrp;
		return 0;
	case TIOCSPGRP:
		p->pgrp = *(unsigned *)buf;
		return 0;
	case TIOCSCTTY: {
		task_struct *cur = CURRENT_TASK();
		int steal = buf ? *(int *)buf : 0;
		if (!cur->user || cur->user->session_id != cur->psid)
			return -EPERM;
		if (p->pgrp && !steal)
			return -EPERM;
		p->pgrp = cur->user->group_id;
		return 0;
	}
	case KDGETMODE:
		*(int *)buf = KD_TEXT;
		return 0;
	case KDSETMODE:
		return 0;
	case GIO_FONT:
		memset(buf, 0, 256 * 8);
		return 0;
	case PIO_FONT:
		return 0;
	case GIO_FONTX: {
		struct consolefontdesc *cfd = (struct consolefontdesc *)buf;
		if (cfd->chardata)
			memset(cfd->chardata, 0,
			       (size_t)cfd->charcount * cfd->charheight);
		cfd->charcount = 256;
		cfd->charheight = 16;
		return 0;
	}
	case PIO_FONTX:
		return 0;
	case KDFONTOP: {
		struct console_font_op *cfo = (struct console_font_op *)buf;
		switch (cfo->op) {
		case KD_FONT_OP_SET:
		case KD_FONT_OP_SET_DEFAULT:
		case KD_FONT_OP_COPY:
			return 0;
		case KD_FONT_OP_GET:
			cfo->width = 8;
			cfo->height = 16;
			cfo->charcount = 256;
			if (cfo->data)
				memset(cfo->data, 0,
				       cfo->charcount * ((cfo->width + 7) / 8) *
					       cfo->height);
			return 0;
		}
		return -EINVAL;
	}
	case PIO_UNIMAPCLR:
	case PIO_UNIMAP:
		return 0;
	case GIO_UNIMAP: {
		struct unimapdesc *ud = (struct unimapdesc *)buf;
		ud->entry_ct = 0;
		return 0;
	}
	}
	return -ENOSYS;
}

static int pts_slave_release(file *fp)
{
	pts_pair *p = fp->f_inode->i_private;

	if (__sync_add_and_fetch(&p->slave_count, -1) == 0) {
		cyb_writer_close(p->s2m);
		cyb_reader_close(p->m2s);
		pts_pair_check_free(p);
	}

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static int pts_slave_getattr(inode *node, struct stat *s)
{
	pts_pair *p = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(PTS_MAJOR, 0);
	s->st_rdev = MKDEV(PTS_MAJOR, p->idx);
	s->st_ino = PTS_INO_MASK | (uint64_t)p->idx;
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = PAGE_SIZE;
	s->st_size = PAGE_SIZE;
	s->st_blocks = 1;
	return 0;
}

static const inode_operations pts_slave_iops = {
	.getattr = pts_slave_getattr,
};

static void pts_slave_poll_wait(file *fp, task_struct *task)
{
	pts_pair *p = fp->f_inode->i_private;
	cyb_set_poll_read(p->m2s, task);
}

static void pts_slave_poll_wait_remove(file *fp, task_struct *task)
{
	pts_pair *p = fp->f_inode->i_private;
	(void)task;
	cyb_clear_poll_read(p->m2s);
}

static const file_operations pts_slave_fops = {
	.release = pts_slave_release,
	.read = pts_slave_read,
	.write = pts_slave_write,
	.is_ready = pts_slave_poll,
	.poll_wait = pts_slave_poll_wait,
	.poll_wait_remove = pts_slave_poll_wait_remove,
	.ioctl = pts_slave_ioctl,
};

/* O_PATH open: metadata only, no cycbuf refs taken. */
static int pts_slave_path_release(file *fp)
{
	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const file_operations pts_slave_path_fops = {
	.release = pts_slave_path_release,
};

/* ── Master file operations ───────────────────────────────────────────────── */

static ssize_t pts_master_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	if (!buf || size < 1)
		return 0;
	return (ssize_t)cyb_getbuf(p->s2m, buf, (int)size, 1, 1);
}

static ssize_t pts_master_write(file *fp, const void *buf, size_t size,
				loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	if (!buf || size < 1)
		return 0;
	if (cyb_reader_count(p->m2s) == 0)
		return -EIO;
	cyb_putbuf(p->m2s, (unsigned char *)buf, (unsigned)size, 0, 0);
	return (ssize_t)size;
}

static int pts_master_poll(file *fp, unsigned type)
{
	pts_pair *p = fp->f_inode->i_private;
	if (type == FS_POLL_READ)
		return cyb_isempty(p->s2m) ? -1 : 0;
	if (type == FS_POLL_WRITE)
		return 0;
	return -1;
}

static int pts_master_ioctl(file *fp, unsigned cmd, void *buf)
{
	pts_pair *p = fp->f_inode->i_private;
	switch (cmd) {
	case TIOCGPTN:
		*(unsigned *)buf = (unsigned)p->idx;
		return 0;
	case TIOCSPTLCK:
		/* lock field not stored in BSD mode; accept silently */
		return 0;
	case TIOCGPTLCK:
		*(int *)buf = 0;
		return 0;
	case TCGETS:
		memcpy(buf, &p->termios, sizeof(p->termios));
		return 0;
	case TCSETS:
	case TCSETSW:
		memcpy(&p->termios, buf, sizeof(p->termios));
		return 0;
	case TCSETSF:
		p->canon.len = 0;
		cyb_flush(p->m2s);
		memcpy(&p->termios, buf, sizeof(p->termios));
		return 0;
	case TIOCGWINSZ:
		memcpy(buf, &p->winsize, sizeof(p->winsize));
		return 0;
	case TIOCSWINSZ:
		memcpy(&p->winsize, buf, sizeof(p->winsize));
		return 0;
	case TIOCGPGRP:
		*(unsigned *)buf = p->pgrp;
		return 0;
	case TIOCSPGRP:
		p->pgrp = *(unsigned *)buf;
		return 0;
	case TIOCSCTTY: {
		task_struct *cur = CURRENT_TASK();
		int steal = buf ? *(int *)buf : 0;
		if (!cur->user || cur->user->session_id != cur->psid)
			return -EPERM;
		if (p->pgrp && !steal)
			return -EPERM;
		p->pgrp = cur->user->group_id;
		return 0;
	}
	}
	return -ENOSYS;
}

static int pts_master_release(file *fp)
{
	pts_pair *p = fp->f_inode->i_private;

	cyb_writer_close(p->m2s);
	cyb_reader_close(p->s2m);

	p->master_open = 0;
	pts_pair_check_free(p);

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static int pts_master_getattr(inode *node, struct stat *s)
{
	pts_pair *p = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(PTM_MAJOR, 0);
	s->st_rdev = MKDEV(PTM_MAJOR, p->idx);
	s->st_ino = PTS_INO_MASK | (uint64_t)(MAX_PTS + p->idx);
	s->st_nlink = 1;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = PAGE_SIZE;
	s->st_size = PAGE_SIZE;
	s->st_blocks = 1;
	return 0;
}

static const inode_operations pts_master_iops = {
	.getattr = pts_master_getattr,
};

static void pts_master_poll_wait(file *fp, task_struct *task)
{
	pts_pair *p = fp->f_inode->i_private;
	cyb_set_poll_read(p->s2m, task);
}

static void pts_master_poll_wait_remove(file *fp, task_struct *task)
{
	pts_pair *p = fp->f_inode->i_private;
	(void)task;
	cyb_clear_poll_read(p->s2m);
}

static const file_operations pts_master_fops = {
	.release = pts_master_release,
	.read = pts_master_read,
	.write = pts_master_write,
	.is_ready = pts_master_poll,
	.poll_wait = pts_master_poll_wait,
	.poll_wait_remove = pts_master_poll_wait_remove,
	.ioctl = pts_master_ioctl,
};

/* ── cdev open callbacks ──────────────────────────────────────────────────── */

/*
 * ptm_cdev_open — open /dev/ptypN (master).
 * Allocates and initialises pair N; fails with EBUSY if already in use.
 */
static file *ptm_cdev_open(unsigned rdev, int flag)
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

/*
 * ptmx_cdev_open — open /dev/ptmx (Unix98).
 * Dynamically allocates any free pair; use TIOCGPTN to discover the index.
 */
static file *ptmx_cdev_open(unsigned rdev, int flag)
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

/*
 * pts_cdev_open — open /dev/ttypN (slave).
 * Requires the master to be open; fails if pair is unused or locked.
 */
static file *pts_cdev_open(unsigned rdev, int flag)
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

/* ── /dev/pts directory superblock ───────────────────────────────────────── */

/*
 * Unix98 PTY slaves are accessed via /dev/pts/N.
 * grantpt(3) calls ptsname_r() -> TIOCGPTN to get N, then stat("/dev/pts/N")
 * to verify ownership.  We provide a minimal directory superblock mounted at
 * /dev/pts with pre-created slave device nodes /dev/pts/0 .. /dev/pts/15.
 */

static int ptsdir_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFDIR | 0755;
	s->st_nlink = 2;
	s->st_ino = PTS_INO_MASK | (MAX_PTS * 2 + 1);
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_blksize = PAGE_SIZE;
	s->st_size = PAGE_SIZE;
	s->st_blocks = 1;
	return 0;
}

static int ptsdir_release(file *fp)
{
	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const inode_operations ptsdir_iops = {
	.getattr = ptsdir_getattr,
};

static const file_operations ptsdir_fops = {
	.release = ptsdir_release,
};

static file *ptsdir_open_root(super_block *sb, int flag)
{
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	node->i_mode = S_IFDIR | 0755;
	node->i_op = &ptsdir_iops;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &ptsdir_fops;
	return fp;
}

static void ptsdir_release_super(super_block *sb)
{
	kfree(sb);
}

static const super_operations ptsdir_sops = {
	.open_root = ptsdir_open_root,
	.release = ptsdir_release_super,
};

/*
 * RH9 BSD-style PTY nodes:
 *   /dev/ptyp0 … /dev/ptypf  (masters, major 2, minor 0-15)
 *   /dev/ttyp0 … /dev/ttypf  (slaves,  major 3, minor 0-15)
 *
 * Unix98 PTY nodes:
 *   /dev/ptmx            (multiplexor master)
 *   /dev/pts/0 … /dev/pts/15  (slaves, major 3, minor 0-15)
 */
static const char pty_hex[] = "0123456789abcdef";

static void pts_dev_register(super_block *dev_sb)
{
	char path[16];
	int i;

	printk("dev: registered /dev/pts master and slavers\n");

	spinlock_init(&pts_alloc_lock);

	cdev_register(S_IFCHR, PTM_MAJOR, 0, MAX_PTS, ptm_cdev_open);
	cdev_register(S_IFCHR, PTS_MAJOR, 0, MAX_PTS, pts_cdev_open);
	cdev_register(S_IFCHR, PTMX_MAJOR, PTMX_MINOR, 1, ptmx_cdev_open);

	vfs_mknod(dev_sb, "/ptmx", S_IFCHR | 0666,
		  MKDEV(PTMX_MAJOR, PTMX_MINOR));

	for (i = 0; i < MAX_PTS; i++) {
		sprintf(path, "/ptyp%c", pty_hex[i]);
		vfs_mknod(dev_sb, path, S_IFCHR | 0620, MKDEV(PTM_MAJOR, i));

		sprintf(path, "/ttyp%c", pty_hex[i]);
		vfs_mknod(dev_sb, path, S_IFCHR | 0620, MKDEV(PTS_MAJOR, i));
	}

	/* Unix98 /dev/pts/N slave nodes — sb is exposed via pts_get_devpts_sb(). */
	ptsdir_sb_global = sget(&ptsdir_sops);
	for (i = 0; i < MAX_PTS; i++) {
		sprintf(path, "/%d", i);
		vfs_mknod(ptsdir_sb_global, path, S_IFCHR | 0620,
			  MKDEV(PTS_MAJOR, i));
	}
}

DEV_INIT(pts_dev_register);

/*
 * pts_get_devpts_sb — return the pre-built /dev/pts superblock.
 *
 * Called by devpts_get_sb() in mount.c when userspace (or kinit_userspace)
 * mounts "devpts" on "/dev/pts".  The superblock is created once by
 * pts_dev_register() at DEV_INIT time; this just hands back a reference.
 */
super_block *pts_get_devpts_sb(void)
{
	if (!ptsdir_sb_global)
		return NULL;
	sb_get(ptsdir_sb_global);
	return ptsdir_sb_global;
}
