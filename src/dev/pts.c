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

void pts_pair_check_free(pts_pair *p, spinlock_t *lock)
{
	cy_buf *m2s = NULL, *s2m = NULL;
	int irq;

	spinlock_lock(lock, &irq);
	if (!p->master_open && p->slave_count == 0) {
		p->used = 0;
		/* Drop the slave-side refs that were pre-allocated by cyb_create
		 * but never consumed (no slave ever opened this pair). */
		m2s = p->m2s;
		s2m = p->s2m;
		p->m2s = p->s2m = NULL;
	}
	spinlock_unlock(lock, irq);

	if (m2s) {
		cyb_reader_close(m2s);
		cyb_writer_close(s2m);
	}
}

int pts_master_ioctl(file *fp, unsigned cmd, void *buf)
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

ssize_t pts_master_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	if (!buf || size < 1)
		return 0;
	return (ssize_t)cyb_getbuf(p->s2m, buf, (int)size, 1, 1);
}

ssize_t pts_master_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	if (!buf || size < 1)
		return 0;
	if (cyb_reader_count(p->m2s) == 0)
		return -EIO;
	cyb_putbuf(p->m2s, (unsigned char *)buf, (unsigned)size, 0, 0);
	return (ssize_t)size;
}

int pts_master_poll(file *fp, unsigned type)
{
	pts_pair *p = fp->f_inode->i_private;
	if (type == FS_POLL_READ)
		return cyb_isempty(p->s2m) ? -1 : 0;
	if (type == FS_POLL_WRITE)
		return 0;
	return -1;
}

void pts_master_poll_wait(file *fp, task_struct *task)
{
	pts_pair *p = fp->f_inode->i_private;
	cyb_set_poll_read(p->s2m, task);
}

void pts_master_poll_wait_remove(file *fp, task_struct *task)
{
	pts_pair *p = fp->f_inode->i_private;
	(void)task;
	cyb_clear_poll_read(p->s2m);
}

int pts_slave_ioctl(file *fp, unsigned cmd, void *buf)
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

int pts_canon_readline(pts_pair *p)
{
	return tty_ldisc_canon_readline(&p->canon, &p->termios, p->m2s, 1,
					p->pgrp, NULL, NULL);
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

ssize_t pts_slave_write(file *fp, const void *buf, size_t size, loff_t *pos)
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

int pts_slave_poll(file *fp, unsigned type)
{
	pts_pair *p = fp->f_inode->i_private;
	if (type == FS_POLL_READ)
		return cyb_isempty(p->m2s) ? -1 : 0;
	if (type == FS_POLL_WRITE)
		return 0;
	return -1;
}

void pts_slave_poll_wait(file *fp, task_struct *task)
{
	pts_pair *p = fp->f_inode->i_private;
	cyb_set_poll_read(p->m2s, task);
}

void pts_slave_poll_wait_remove(file *fp, task_struct *task)
{
	pts_pair *p = fp->f_inode->i_private;
	(void)task;
	cyb_clear_poll_read(p->m2s);
}