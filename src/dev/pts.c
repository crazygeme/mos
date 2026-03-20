/*
 * src/dev/pts.c - pseudo-terminal slave (pts) filesystem
 *
 * Implements /dev/ptmx (master multiplexer) and /dev/pts/N (slave devices).
 *
 * PTY pair mechanics:
 *   m2s: master writes → slave reads  (with slave line discipline)
 *   s2m: slave writes  → master reads (raw)
 *
 * Typical usage:
 *   fd  = open("/dev/ptmx", O_RDWR)   -- allocate PTY, get master fd
 *   ioctl(fd, TIOCGPTN, &n)           -- get slave index N
 *   sfd = open("/dev/pts/N", O_RDWR)  -- open slave
 *
 * /dev/ptmx is mounted at boot (it is a single device node, always present).
 * /dev/pts  is mounted dynamically: userspace must call
 *   mount("devpts", "/dev/pts", "devpts", 0, NULL)
 * which invokes devpts_get_sb() and vfs_mount() via fs_do_mount().
 */

#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
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

/* ── Linux-compatible ioctl numbers ──────────────────────────────────────── */

#define TIOCGPTN 0x80045430 /* get pty number */
#define TIOCSPTLCK 0x40045431 /* lock/unlock slave */
#define TIOCGPTLCK 0x80045439 /* query slave lock state */

/* ── Configuration ────────────────────────────────────────────────────────── */

#define MAX_PTS 16

/* ── Per-PTY state ────────────────────────────────────────────────────────── */

typedef struct {
	int idx; /* index in pts_pairs[] (= slave minor number) */
	int used; /* 1 while this slot is allocated */
	spinlock_t lock;

	/* terminal attributes */
	struct termios termios;
	struct winsize winsize;
	unsigned pgrp;

	/*
	 * Two cyclebufs form the PTY pipe:
	 *   m2s: master is writer, slave is reader
	 *   s2m: slave is writer, master is reader
	 *
	 * Lifetime:  cyb_create() sets writer_count = reader_count = 1.
	 *   master close → cyb_writer_close(m2s) + cyb_reader_close(s2m)
	 *   last slave close → cyb_writer_close(s2m) + cyb_reader_close(m2s)
	 * Each cyclebuf therefore receives exactly one writer_close and one
	 * reader_close, matching the ref_count of 2 set by cyb_create().
	 */
	cy_buf *m2s;
	cy_buf *s2m;

	/* canonical mode line buffer for slave reads */
	tty_canon_t canon;

	/* open-count tracking (protected by pts_alloc_lock for the 0→freed
	 * transition; slave_count itself is updated atomically) */
	int master_open; /* 1 while master fd is open */
	int slave_count; /* number of open slave fds */

	int locked; /* 0 = slave may be opened, 1 = locked (TIOCSPTLCK) */
} pts_pair;

static pts_pair pts_pairs[MAX_PTS];
static spinlock_t pts_alloc_lock;

/* ── Pair allocation / recycling ──────────────────────────────────────────── */

static pts_pair *pts_alloc(void)
{
	int i;
	spinlock_lock(&pts_alloc_lock);
	for (i = 0; i < MAX_PTS; i++) {
		if (!pts_pairs[i].used) {
			pts_pair *p = &pts_pairs[i];
			memset(p, 0, sizeof(*p));
			p->idx = i;
			p->used = 1;
			p->master_open = 1;
			p->slave_count = 0;
			p->locked = 0; /* slave immediately openable */
			p->termios = tty_default_termios;
			p->winsize.ws_row = 24;
			p->winsize.ws_col = 80;
			spinlock_init(&p->lock);
			p->m2s = cyb_create();
			p->s2m = cyb_create();
			spinlock_unlock(&pts_alloc_lock);
			return p;
		}
	}
	spinlock_unlock(&pts_alloc_lock);
	return NULL;
}

/* Mark the slot free once both sides have closed. */
static void pts_pair_check_free(pts_pair *p)
{
	spinlock_lock(&pts_alloc_lock);
	if (!p->master_open && p->slave_count == 0)
		p->used = 0;
	spinlock_unlock(&pts_alloc_lock);
}

/* ── Slave line discipline helpers ────────────────────────────────────────── */

/*
 * pts_canon_readline - blocking read of one line from m2s into p->canon.buf.
 * Returns 1 if a line was assembled, 0 on EOF (master closed with no data).
 */
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
	return (ssize_t)cyb_getbuf(p->m2s, buf, (int)size);
}

/*
 * Slave write → s2m with optional OPOST output processing.
 * The master reads raw bytes from s2m.
 */
static ssize_t pts_slave_write(file *fp, const void *buf, size_t size,
			       loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	const unsigned char *src = (const unsigned char *)buf;
	unsigned i;
	if (!buf || size < 1)
		return 0;
	/* Master has closed: no reader on s2m. */
	if (cyb_reader_count(p->s2m) == 0)
		return -EIO;
	for (i = 0; i < (unsigned)size; i++) {
		unsigned char c = src[i];
		if ((p->termios.c_oflag & OPOST) && c == '\n' &&
		    (p->termios.c_oflag & ONLCR)) {
			cyb_putc(p->s2m, '\r');
		}
		cyb_putc(p->s2m, c);
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
	}
	return -ENOSYS;
}

static int pts_slave_release(file *fp)
{
	pts_pair *p = fp->f_inode->i_private;

	if (__sync_add_and_fetch(&p->slave_count, -1) == 0) {
		/*
		 * Last slave close: signal EOF on the slave→master direction
		 * and release the slave's hold on m2s.
		 */
		cyb_writer_close(p->s2m); /* master reads now get EOF */
		cyb_reader_close(p->m2s); /* drop slave's reader ref */
		pts_pair_check_free(p);
	}

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static int pts_slave_getattr(inode *node, struct stat *s)
{
	pts_pair *p = node->i_private;
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0x88;
	s->st_gid = 0;
	s->st_ino = (uint64_t)p->idx;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_nlink = 1;
	s->st_rdev = (unsigned)(0x8800 | p->idx);
	s->st_size = 0;
	return 0;
}

static const inode_operations pts_slave_iops = {
	.getattr = pts_slave_getattr,
};

static const file_operations pts_slave_fops = {
	.release = pts_slave_release,
	.read = pts_slave_read,
	.write = pts_slave_write,
	.poll = pts_slave_poll,
	.ioctl = pts_slave_ioctl,
};

/* ── Master file operations ───────────────────────────────────────────────── */

/*
 * Master read: drain bytes that the slave wrote into s2m.
 * Blocks until data is available or the slave closes (EOF).
 */
static ssize_t pts_master_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	if (!buf || size < 1)
		return 0;
	return (ssize_t)cyb_getbuf(p->s2m, buf, (int)size);
}

/*
 * Master write: push bytes into m2s; the slave's read() will process them
 * through its line discipline.
 */
static ssize_t pts_master_write(file *fp, const void *buf, size_t size,
				loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	if (!buf || size < 1)
		return 0;
	/* All slaves have closed: no reader on m2s. */
	if (cyb_reader_count(p->m2s) == 0)
		return -EIO;
	cyb_putbuf(p->m2s, (unsigned char *)buf, (unsigned)size);
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
		p->locked = *(int *)buf;
		return 0;
	case TIOCGPTLCK:
		*(int *)buf = p->locked;
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

	/*
	 * Signal EOF to the slave side and release the master's hold on s2m.
	 * Order: writer_close before reader_close so a blocked slave read
	 * wakes up and sees EOF before s2m is potentially destroyed.
	 */
	cyb_writer_close(p->m2s); /* slave reads now get EOF */
	cyb_reader_close(p->s2m); /* drop master's reader ref */

	p->master_open = 0;
	pts_pair_check_free(p);

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static int pts_master_getattr(inode *node, struct stat *s)
{
	pts_pair *p = node->i_private;
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0x88;
	s->st_gid = 0;
	s->st_ino = (uint64_t)(MAX_PTS + p->idx);
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_nlink = 1;
	s->st_rdev = (unsigned)(0x8880 | p->idx);
	s->st_size = 0;
	return 0;
}

static const inode_operations pts_master_iops = {
	.getattr = pts_master_getattr,
};

static const file_operations pts_master_fops = {
	.release = pts_master_release,
	.read = pts_master_read,
	.write = pts_master_write,
	.poll = pts_master_poll,
	.ioctl = pts_master_ioctl,
};

/* ── /dev/ptmx super_block: allocates master on open ─────────────────────── */

static file *ptmx_open_root(super_block *sb)
{
	pts_pair *p = pts_alloc();
	if (!p)
		return NULL;

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

static super_operations ptmx_sops = {
	.open_root = ptmx_open_root,
};

/* ── /dev/pts super_block: opens slave by index ───────────────────────────── */

/*
 * pts_sb_open - called by VFS with path relative to /dev/pts, e.g. "/0".
 */
static file *pts_sb_open(super_block *sb, const char *path, int flag)
{
	int idx;
	pts_pair *p;

	if (!path || path[0] != '/')
		return NULL;

	idx = atoi(path + 1);
	if (idx < 0 || idx >= MAX_PTS)
		return NULL;

	p = &pts_pairs[idx];

	spinlock_lock(&pts_alloc_lock);
	if (!p->used || !p->master_open || p->locked) {
		spinlock_unlock(&pts_alloc_lock);
		return NULL;
	}
	__sync_add_and_fetch(&p->slave_count, 1);
	spinlock_unlock(&pts_alloc_lock);

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	node->i_op = &pts_slave_iops;
	node->i_private = p;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &pts_slave_fops;
	return fp;
}

static super_operations pts_dir_sops = {
	.open = pts_sb_open,
};

/* ── devpts filesystem type ───────────────────────────────────────────────── */

/*
 * devpts_get_sb — called by fs_do_mount() when userspace mounts "devpts".
 * Returns a fresh super_block backed by pts_dir_sops so that VFS routes
 * opens under /dev/pts to pts_sb_open().
 */
static super_block *devpts_get_sb(const char *dev, const char *target,
				  int flags, void *data)
{
	return sget(&pts_dir_sops);
}

static fs_type devpts_fs_type = { .name = "devpts", .get_sb = devpts_get_sb };

/* ── Initialization ───────────────────────────────────────────────────────── */

static void pts_dev_init(void)
{
	spinlock_init(&pts_alloc_lock);

	/* Register devpts so that sys_mount("devpts", "/dev/pts", ...) works */
	fs_register_type(&devpts_fs_type);
}

static void pts_dev_register(super_block *dev_sb)
{
	/* /dev/ptmx — master multiplexer */
	vfs_mount(dev_sb, "/ptmx", sget(&ptmx_sops));

	/* /dev/pts — slave directory, auto-mounted at boot */
	vfs_mount(dev_sb, "/pts", sget(&pts_dir_sops));
}

KERNEL_INIT(5, pts_dev_init);
DEV_INIT(pts_dev_register);
