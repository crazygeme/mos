/*
 * src/fs/mount/pts.c - pseudo-terminal slave (pts) filesystem
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
 */

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
	const struct termios *tc = &p->termios;
	while (1) {
		unsigned char raw = cyb_getc(p->m2s);
		if (raw == (unsigned char)EOF)
			return p->canon.len > 0 ? 1 : 0;

		int ch = tty_input_translate(raw, tc->c_iflag);
		if (ch < 0)
			continue;
		unsigned char c = (unsigned char)ch;

		/* Signals (stub: discard) */
		if ((tc->c_lflag & ISIG) && tc->c_cc[VINTR] &&
		    c == tc->c_cc[VINTR])
			continue;

		if (tc->c_cc[VEOF] && c == tc->c_cc[VEOF])
			return p->canon.len > 0 ? 1 : 0;

		if (tc->c_cc[VERASE] && c == tc->c_cc[VERASE]) {
			if (p->canon.len > 0)
				p->canon.len--;
			continue;
		}

		if (tc->c_cc[VKILL] && c == tc->c_cc[VKILL]) {
			p->canon.len = 0;
			continue;
		}

		if (p->canon.len < TTY_CANON_BUF_SIZE - 1)
			p->canon.buf[p->canon.len++] = (char)c;

		if (tty_is_eol(c, tc))
			return 1;
	}
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
	case TCSETSF:
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
	case TCSETSF:
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

	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	node->i_op = &pts_master_iops;
	node->i_private = p;

	file *fp = calloc(1, sizeof(*fp));
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

	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	node->i_op = &pts_slave_iops;
	node->i_private = p;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &pts_slave_fops;
	return fp;
}

static super_operations pts_dir_sops = {
	.open = pts_sb_open,
};

/* ── Initialization ───────────────────────────────────────────────────────── */

static void pts_fs_init(void)
{
	task_struct *cur = CURRENT_TASK();
	super_block *sb;

	spinlock_init(&pts_alloc_lock);

	/* /dev/ptmx – master multiplexer device */
	sb = sget(&ptmx_sops);
	vfs_mount(cur->root, "/dev/ptmx", sb);

	/* /dev/pts – slave device directory */
	sb = sget(&pts_dir_sops);
	vfs_mount(cur->root, "/dev/pts", sb);
}

KERNEL_INIT(5, pts_fs_init);
