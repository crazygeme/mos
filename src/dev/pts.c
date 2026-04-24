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
#include "pts_internal.h"

static int pts_file_nonblock(file *fp)
{
	return (fp->f_flag & O_NONBLOCK) != 0;
}

static int pts_slave_fionread(pts_pair *p)
{
	return cyb_get_buf_len(p->m2s);
}

void pts_pair_check_free(pts_pair *p, spinlock_t *lock)
{
	cy_buf *m2s = NULL, *s2m = NULL;
	int irq;

	spinlock_lock(lock, &irq);
	if (!p->master_open && p->slave_count == 0) {
		p->used = 0;
		m2s = p->m2s;
		s2m = p->s2m;
		p->m2s = p->s2m = NULL;
	}
	spinlock_unlock(lock, irq);

	if (m2s) {
		cyb_destroy(m2s);
		cyb_destroy(s2m);
	}
}

int pts_slave_setattr(file *fp, uint32_t mode)
{
	inode *node = fp->f_inode;
	pts_pair *p = node->i_private;
	int irq;

	spinlock_lock(&p->lock, &irq);
	p->slave_mode = (p->slave_mode & S_IFMT) | (mode & ~S_IFMT);
	node->i_mode = p->slave_mode;
	spinlock_unlock(&p->lock, irq);
	return 0;
}

int pts_slave_chown(file *fp, uint32_t uid, uint32_t gid)
{
	inode *node = fp->f_inode;
	pts_pair *p = node->i_private;
	int irq;

	spinlock_lock(&p->lock, &irq);
	if (uid != (uint32_t)-1)
		p->slave_uid = uid;
	if (gid != (uint32_t)-1)
		p->slave_gid = gid;
	spinlock_unlock(&p->lock, irq);
	return 0;
}

/* ioctl cases shared between master and slave */
static int pts_pair_ioctl(pts_pair *p, unsigned cmd, void *buf)
{
	switch (cmd) {
	case TCGETS:
		memcpy(buf, &p->termios, sizeof(p->termios));
		return 0;
	case TCXONC:
		/* PTYs don't model software flow-control stop/start state yet. */
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
		int steal = (int)(uintptr_t)buf;
		if (!cur->user || cur->user->session_id != cur->psid)
			return -EPERM;
		if (p->pgrp && !steal)
			return -EPERM;
		p->pgrp = cur->user->group_id;
		return 0;
	}
	case TIOCNOTTY: {
		task_struct *cur = CURRENT_TASK();
		if (cur->user && p->pgrp == cur->user->group_id)
			p->pgrp = 0;
		return 0;
	}
	}
	return -ENOSYS;
}

int pts_master_ioctl(file *fp, unsigned cmd, void *buf)
{
	pts_pair *p = fp->f_inode->i_private;
	int ret = pts_pair_ioctl(p, cmd, buf);
	if (ret != -ENOSYS)
		return ret;
	switch (cmd) {
	case TIOCGPTN:
		*(unsigned *)buf = (unsigned)p->idx;
		return 0;
	case FIONREAD:
		*(int *)buf = cyb_get_buf_len(p->s2m);
		return 0;
	case TIOCSPTLCK:
		p->pt_locked = buf ? (*(int *)buf != 0) : 0;
		return 0;
	case TIOCGPTLCK:
		*(int *)buf = p->pt_locked;
		return 0;
	case TCFLSH: {
		int sel = (int)(uintptr_t)buf;
		if (sel != TCIFLUSH && sel != TCOFLUSH && sel != TCIOFLUSH)
			sel = TCIOFLUSH;
		if (sel == TCIFLUSH || sel == TCIOFLUSH)
			cyb_flush(p->s2m);
		if (sel == TCOFLUSH || sel == TCIOFLUSH)
			cyb_flush(p->m2s);
		return 0;
	}
	case TIOCPKT:
		p->pkt_mode = buf ? (*(int *)buf != 0) : 0;
		p->pkt_status = 0;
		return 0;
	}
	return -ENOSYS;
}

ssize_t pts_master_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	unsigned char hdr = TIOCPKT_DATA;
	int nonblock = pts_file_nonblock(fp);
	int n;

	if (!buf || size < 1)
		return 0;

	if (p->pkt_mode) {
		if (p->pkt_status) {
			*(unsigned char *)buf = p->pkt_status;
			p->pkt_status = 0;
			return 1;
		}
		if (size < 2)
			return -EINVAL;
		n = cyb_getbuf(p->s2m, (unsigned char *)buf + 1, (int)size - 1,
			       !nonblock, 1);
		if (nonblock && n == 0 && cyb_writer_count(p->s2m) > 0)
			return -EAGAIN;
		if (n <= 0)
			return (ssize_t)n;
		*(unsigned char *)buf = hdr;
		return (ssize_t)(n + 1);
	}

	n = cyb_getbuf(p->s2m, buf, (int)size, !nonblock, 1);
	if (nonblock && n == 0 && cyb_writer_count(p->s2m) > 0)
		return -EAGAIN;
	return (ssize_t)n;
}

ssize_t pts_master_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	int nonblock = pts_file_nonblock(fp);
	int ret;
	if (!buf || size < 1)
		return 0;
	if (cyb_reader_count(p->m2s) == 0)
		return -EIO;
	ret = cyb_putbuf(p->m2s, (unsigned char *)buf, (unsigned)size,
			 !nonblock, 1);
	if (ret == -EPIPE)
		return -EIO;
	if (ret < 0)
		return -EINTR;
	if (nonblock && ret == 0 && cyb_reader_count(p->m2s) > 0)
		return -EAGAIN;
	return (ssize_t)ret;
}

unsigned pts_master_poll(file *fp, unsigned events, poll_table *pt)
{
	pts_pair *p = fp->f_inode->i_private;
	unsigned ready = 0;
	if ((events & FS_POLL_READ) && (p->pkt_status || !cyb_isempty(p->s2m)))
		ready |= FS_POLL_READ;
	if ((events & FS_POLL_HUP) && p->slave_ever_opened &&
	    cyb_writer_count(p->s2m) == 0)
		ready |= FS_POLL_HUP;
	if (events & FS_POLL_WRITE)
		ready |= FS_POLL_WRITE;
	if (!ready && pt && (events & (FS_POLL_READ | FS_POLL_HUP)))
		cyb_poll_read(p->s2m, pt);
	return ready;
}

int pts_slave_ioctl(file *fp, unsigned cmd, void *buf)
{
	pts_pair *p = fp->f_inode->i_private;
	int ret = pts_pair_ioctl(p, cmd, buf);
	if (ret != -ENOSYS)
		return ret;
	switch (cmd) {
	case TCFLSH: {
		int sel = (int)(uintptr_t)buf;
		if (sel != TCIFLUSH && sel != TCOFLUSH && sel != TCIOFLUSH)
			sel = TCIOFLUSH;
		if (sel == TCIFLUSH || sel == TCIOFLUSH)
			cyb_flush(p->m2s);
		if (sel == TCOFLUSH || sel == TCIOFLUSH)
			cyb_flush(p->s2m);
		return 0;
	}
	case FIONREAD:
		*(int *)buf = pts_slave_fionread(p);
		return 0;
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

/* O_PATH open: metadata only, no cycbuf refs taken. */
int pts_slave_path_release(file *fp)
{
	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

ssize_t pts_slave_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	int nonblock = pts_file_nonblock(fp);
	int n;
	if (!buf || size < 1)
		return 0;
	if (p->termios.c_lflag & ICANON) {
		if (p->canon.len == 0) {
			n = tty_ldisc_canon_readline(&p->canon, &p->termios,
						     p->m2s, !nonblock, 1,
						     p->pgrp, NULL, NULL);
			if (n < 0)
				return -EINTR;
			if (n == 0) {
				if (nonblock && cyb_writer_count(p->m2s) > 0)
					return -EAGAIN;
				return 0;
			}
		}
		return (ssize_t)tty_canon_drain(&p->canon, (char *)buf,
						(int)size);
	}
	n = cyb_getbuf(p->m2s, buf, (int)size, !nonblock, 1);
	if (n < 0)
		return -EINTR;
	if (nonblock && n == 0 && cyb_writer_count(p->m2s) > 0)
		return -EAGAIN;
	return (ssize_t)n;
}

/*
 * Interprets a cyb_putbuf result during a slave write.
 * Returns 1 (stop) and fills *out if the write was incomplete or failed.
 * Returns 0 (continue) on full success.
 */
static int pts_write_check(int ret, size_t consumed, int nonblock, cy_buf *s2m,
			   unsigned expected, ssize_t *out)
{
	if (ret == -EPIPE) {
		*out = consumed > 0 ? (ssize_t)consumed : -EIO;
		return 1;
	}
	if (ret < 0) {
		*out = consumed > 0 ? (ssize_t)consumed : -EINTR;
		return 1;
	}
	if ((unsigned)ret < expected) {
		*out = (nonblock && consumed == 0 &&
			cyb_reader_count(s2m) > 0) ?
			       -EAGAIN :
			       (ssize_t)consumed;
		return 1;
	}
	return 0;
}

/* Translate \n → \r\n on the way to the master (OPOST|ONLCR). */
static ssize_t pts_slave_write_opost(pts_pair *p, const unsigned char *src,
				     size_t size, int nonblock)
{
	static const unsigned char crnl[2] = { '\r', '\n' };
	int blocking = nonblock ? 0 : 1;
	size_t consumed = 0;
	unsigned i = 0;

	while (i < (unsigned)size) {
		unsigned j = i;
		int ret;
		ssize_t out;

		while (j < (unsigned)size && src[j] != '\n')
			j++;

		if (j > i) {
			unsigned chunk = j - i;
			ret = cyb_putbuf(p->s2m, (unsigned char *)(src + i),
					 chunk, blocking, 1);
			if (pts_write_check(ret, consumed, nonblock, p->s2m,
					    chunk, &out))
				return out;
			consumed += (size_t)ret;
		}

		if (j < (unsigned)size) {
			ret = cyb_putbuf(p->s2m, (unsigned char *)crnl, 2,
					 blocking, 1);
			if (pts_write_check(ret, consumed, nonblock, p->s2m, 2,
					    &out))
				return out;
			consumed++;
			j++;
		}
		i = j;
	}
	return (ssize_t)consumed;
}

ssize_t pts_slave_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	int nonblock = pts_file_nonblock(fp);
	int blocking = nonblock ? 0 : 1;
	int ret;

	if (!buf || size < 1)
		return 0;
	if (cyb_reader_count(p->s2m) == 0)
		return -EIO;

	if ((p->termios.c_oflag & OPOST) && (p->termios.c_oflag & ONLCR))
		return pts_slave_write_opost(p, (const unsigned char *)buf,
					     size, nonblock);

	ret = cyb_putbuf(p->s2m, (unsigned char *)buf, (unsigned)size, blocking,
			 1);
	if (ret == -EPIPE)
		return -EIO;
	if (ret < 0)
		return -EINTR;
	if (nonblock && ret == 0 && cyb_reader_count(p->s2m) > 0)
		return -EAGAIN;
	return (ssize_t)ret;
}

unsigned pts_slave_poll(file *fp, unsigned events, poll_table *pt)
{
	pts_pair *p = fp->f_inode->i_private;
	unsigned ready = 0;
	if ((events & FS_POLL_READ) && !cyb_isempty(p->m2s))
		ready |= FS_POLL_READ;
	if ((events & FS_POLL_HUP) && cyb_writer_count(p->m2s) == 0)
		ready |= FS_POLL_HUP;
	if (events & FS_POLL_WRITE)
		ready |= FS_POLL_WRITE;
	if (!ready && pt && (events & (FS_POLL_READ | FS_POLL_HUP)))
		cyb_poll_read(p->m2s, pt);
	return ready;
}
