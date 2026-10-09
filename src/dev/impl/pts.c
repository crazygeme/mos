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
#include <device/time.h>
#include <ps/ps.h>
#include <macro.h>
#include <dev/dev.h>
#include <errno.h>
#include <dev/tty.h>
#include <lib/command.h>
#include <unistd.h>
#include "tty_ldisc.h"
#include "pts_internal.h"

void pts_acquire_controlling(file *fp, int flag)
{
	task_struct *cur = CURRENT_TASK();
	pts_pair *p = fp->f_inode->i_private;
	struct stat s;
	int mask = 0;
	int irq;

	if ((flag & (O_NOCTTY | O_PATH)) || !cur->execution ||
	    cur->thread->session_id != cur->thread->tgid)
		return;
	if (fp->f_fop->getattr(fp, &s))
		return;
	if ((flag & O_ACCMODE) != O_WRONLY)
		mask |= R_OK;
	if ((flag & O_ACCMODE) != O_RDONLY)
		mask |= W_OK;
	if (fs_check_perm(&s, mask) || tty_has_controlling(cur))
		return;

	spinlock_lock(&p->lock, &irq);
	if (!p->pgrp)
		pts_pair_set_group(p, cur->thread->group_id);
	spinlock_unlock(&p->lock, irq);
}

static int pts_file_nonblock(file *fp)
{
	return (fp->f_flag & O_NONBLOCK) != 0;
}

static ssize_t pts_slave_read_raw(pts_pair *p, void *buf, size_t size,
				  int nonblock)
{
	const struct termios *tc = &p->termios;
	unsigned vmin = tc->c_cc[VMIN];
	unsigned char *dst = (unsigned char *)buf;
	int blocking = nonblock ? 0 : 1;
	ssize_t n = 0;

	while ((size_t)n < size) {
		unsigned char raw;
		int ret;
		int ch;

		if ((unsigned)n >= vmin && cyb_isempty(p->m2s))
			break;

		ret = cyb_getbuf(p->m2s, &raw, 1, blocking, 1);
		if (ret < 0)
			return n > 0 ? n : -EINTR;
		if (ret == 0) {
			if (nonblock && n == 0 && cyb_writer_count(p->m2s) > 0)
				return -EAGAIN;
			break;
		}
		if (raw == (unsigned char)EOF)
			break;

		ch = tty_input_translate(raw, tc->c_iflag);
		if (ch < 0)
			continue;
		dst[n++] = (unsigned char)ch;
	}

	return n;
}

static int pts_master_consume_signal_char(pts_pair *p, unsigned char raw)
{
	int ch;

	if (!(p->termios.c_lflag & ISIG) || !p->pgrp)
		return 0;

	ch = tty_input_translate(raw, p->termios.c_iflag);
	if (ch < 0)
		return 0;
	if (!tty_ldisc_handle_signal_char(&p->termios, (unsigned char)ch,
					  p->pgrp))
		return 0;

	/* Match the existing canonical-read behavior: discard the partial
	 * line being assembled when VINTR/VQUIT/VSUSP is received. */
	p->canon.len = 0;
	return 1;
}

static int pts_slave_fionread(pts_pair *p)
{
	return cyb_get_buf_len(p->m2s);
}

void pts_pair_set_group(pts_pair *p, unsigned group)
{
	if (p->group_changed)
		p->group_changed(p, group);
	else
		p->pgrp = group;
}

void pts_pair_close_master(pts_pair *p)
{
	p->master_open = 0;
	if (p->group_changed)
		p->group_changed(p, p->pgrp);
}

void pts_pair_check_free(pts_pair *p, spinlock_t *lock)
{
	cy_buf *m2s = NULL, *s2m = NULL;
	int irq;

	spinlock_lock(lock, &irq);
	if (p->used && !p->master_open && p->slave_count == 0) {
		if (p->on_free)
			p->on_free(p);
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

static unsigned pts_baud_rate(unsigned code, unsigned custom)
{
	static const unsigned rates[] = {
		0,	 50,	  75,	   110,	    134,     150,     200,
		300,	 600,	  1200,	   1800,    2400,    4800,    9600,
		19200,	 38400,	  57600,   115200,  230400,  460800,  500000,
		576000,	 921600,  1000000, 1152000, 1500000, 2000000, 2500000,
		3000000, 3500000, 4000000
	};

	code &= CBAUD;
	if (code == BOTHER)
		return custom;
	if (code & CBAUDEX)
		code = (code & ~CBAUDEX) + 15;
	return rates[code];
}

static void pts_termios_speeds(pts_pair *p)
{
	unsigned input = (p->termios.c_cflag & CIBAUD) >> IBSHIFT;

	p->ospeed = pts_baud_rate(p->termios.c_cflag, p->ospeed);
	p->ispeed = input ? pts_baud_rate(input, p->ispeed) : p->ospeed;
}

/* ioctl cases shared between master and slave */
static int pts_pair_ioctl_tcgets(void *context __attribute__((unused)),
				 unsigned cmd __attribute__((unused)),
				 void *buf __attribute__((unused)))
{
	pts_pair *p = context;
	memcpy(buf, &p->termios, sizeof(p->termios));
	return 0;
}

static int pts_pair_ioctl_tcgets2(void *context __attribute__((unused)),
				  unsigned cmd __attribute__((unused)),
				  void *buf __attribute__((unused)))
{
	pts_pair *p = context;

	struct termios2 *tc = buf;
	pts_termios_speeds(p);
	tc->termios = p->termios;
	tc->c_ispeed = p->ispeed;
	tc->c_ospeed = p->ospeed;
	return 0;
}

static int pts_pair_ioctl_tcsets2(void *context __attribute__((unused)),
				  unsigned cmd __attribute__((unused)),
				  void *buf __attribute__((unused)))
{
	pts_pair *p = context;

	const struct termios2 *tc = buf;
	if (cmd == TCSETSF2) {
		p->canon.len = 0;
		cyb_flush(p->m2s);
	}
	p->termios = tc->termios;
	p->ispeed = tc->c_ispeed;
	p->ospeed = tc->c_ospeed;
	pts_termios_speeds(p);
	return 0;
}

static int pts_pair_ioctl_tcxonc(void *context __attribute__((unused)),
				 unsigned cmd __attribute__((unused)),
				 void *buf __attribute__((unused)))
{
	/* PTYs don't model software flow-control stop/start state yet. */
	return 0;
}

static int pts_pair_ioctl_tcsets(void *context __attribute__((unused)),
				 unsigned cmd __attribute__((unused)),
				 void *buf __attribute__((unused)))
{
	pts_pair *p = context;
	memcpy(&p->termios, buf, sizeof(p->termios));
	return 0;
}

static int pts_pair_ioctl_tcsetsf(void *context __attribute__((unused)),
				  unsigned cmd __attribute__((unused)),
				  void *buf __attribute__((unused)))
{
	pts_pair *p = context;
	p->canon.len = 0;
	cyb_flush(p->m2s);
	memcpy(&p->termios, buf, sizeof(p->termios));
	return 0;
}

static int pts_pair_ioctl_tiocgwinsz(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	pts_pair *p = context;

	int irq;
	spinlock_lock(&p->lock, &irq);
	memcpy(buf, &p->winsize, sizeof(p->winsize));
	spinlock_unlock(&p->lock, irq);
	return 0;
}

static int pts_pair_ioctl_tiocswinsz(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	pts_pair *p = context;

	unsigned pgrp = 0;
	int irq;
	spinlock_lock(&p->lock, &irq);
	if (memcmp(&p->winsize, buf, sizeof(p->winsize)) != 0) {
		memcpy(&p->winsize, buf, sizeof(p->winsize));
		pgrp = p->pgrp;
	}
	spinlock_unlock(&p->lock, irq);
	if (pgrp)
		ps_send_signal_pgrp(pgrp, SIGWINCH);
	return 0;
}

static int pts_pair_ioctl_tiocgpgrp(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf __attribute__((unused)))
{
	pts_pair *p = context;
	*(unsigned *)buf = p->pgrp;
	return 0;
}

static int pts_pair_ioctl_tiocspgrp(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf __attribute__((unused)))
{
	pts_pair *p = context;
	pts_pair_set_group(p, *(unsigned *)buf);
	return 0;
}

static int pts_pair_ioctl_tiocsctty(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf __attribute__((unused)))
{
	pts_pair *p = context;

	task_struct *cur = CURRENT_TASK();
	int steal = (int)(uintptr_t)buf;
	if (!cur->execution || cur->thread->session_id != cur->thread->tgid)
		return -EPERM;
	if (p->pgrp && !steal)
		return -EPERM;
	pts_pair_set_group(p, cur->thread->group_id);
	return 0;
}

static int pts_pair_ioctl_tiocnotty(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf __attribute__((unused)))
{
	pts_pair *p = context;

	task_struct *cur = CURRENT_TASK();
	if (cur->execution && p->pgrp == cur->thread->group_id)
		pts_pair_set_group(p, 0);
	return 0;
}

static const command_operation pts_terminal_commands[256] = {
	[TCGETS & 255] = { TCGETS, pts_pair_ioctl_tcgets },
	[TCGETS2 & 255] = { TCGETS2, pts_pair_ioctl_tcgets2 },
	[TCSETS2 & 255] = { TCSETS2, pts_pair_ioctl_tcsets2 },
	[TCSETSW2 & 255] = { TCSETSW2, pts_pair_ioctl_tcsets2 },
	[TCSETSF2 & 255] = { TCSETSF2, pts_pair_ioctl_tcsets2 },
	[TCXONC & 255] = { TCXONC, pts_pair_ioctl_tcxonc },
	[TCSETS & 255] = { TCSETS, pts_pair_ioctl_tcsets },
	[TCSETSW & 255] = { TCSETSW, pts_pair_ioctl_tcsets },
	[TCSETSF & 255] = { TCSETSF, pts_pair_ioctl_tcsetsf },
	[TIOCGWINSZ & 255] = { TIOCGWINSZ, pts_pair_ioctl_tiocgwinsz },
	[TIOCSWINSZ & 255] = { TIOCSWINSZ, pts_pair_ioctl_tiocswinsz },
	[TIOCGPGRP & 255] = { TIOCGPGRP, pts_pair_ioctl_tiocgpgrp },
	[TIOCSPGRP & 255] = { TIOCSPGRP, pts_pair_ioctl_tiocspgrp },
	[TIOCSCTTY & 255] = { TIOCSCTTY, pts_pair_ioctl_tiocsctty },
	[TIOCNOTTY & 255] = { TIOCNOTTY, pts_pair_ioctl_tiocnotty },
};

static const command_operation *const pts_terminal_command_groups[256] = {
	[(TCGETS >> 8) & 255] = pts_terminal_commands,
};

static int pts_pair_ioctl(pts_pair *p, unsigned cmd, void *buf)
{
	return command_dispatch(pts_terminal_command_groups, p, cmd, buf,
				-ENOSYS);
}

static int pts_master_ioctl_tiocgptn(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	file *fp = context;
	pts_pair *p = fp->f_inode->i_private;
	*(unsigned *)buf = (unsigned)p->idx;
	return 0;
}

static int pts_master_ioctl_fionread(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	file *fp = context;
	pts_pair *p = fp->f_inode->i_private;
	*(int *)buf = cyb_get_buf_len(p->s2m);
	return 0;
}

static int pts_master_ioctl_tiocsptlck(void *context __attribute__((unused)),
				       unsigned cmd __attribute__((unused)),
				       void *buf __attribute__((unused)))
{
	file *fp = context;
	pts_pair *p = fp->f_inode->i_private;
	p->pt_locked = buf ? (*(int *)buf != 0) : 0;
	return 0;
}

static int pts_master_ioctl_tiocgptlck(void *context __attribute__((unused)),
				       unsigned cmd __attribute__((unused)),
				       void *buf __attribute__((unused)))
{
	file *fp = context;
	pts_pair *p = fp->f_inode->i_private;
	*(int *)buf = p->pt_locked;
	return 0;
}

static int pts_master_ioctl_tcflsh(void *context __attribute__((unused)),
				   unsigned cmd __attribute__((unused)),
				   void *buf __attribute__((unused)))
{
	file *fp = context;
	pts_pair *p = fp->f_inode->i_private;

	int sel = (int)(uintptr_t)buf;
	if (sel != TCIFLUSH && sel != TCOFLUSH && sel != TCIOFLUSH)
		sel = TCIOFLUSH;
	if (sel == TCIFLUSH || sel == TCIOFLUSH)
		cyb_flush(p->s2m);
	if (sel == TCOFLUSH || sel == TCIOFLUSH)
		cyb_flush(p->m2s);
	return 0;
}

static int pts_master_ioctl_tiocpkt(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf __attribute__((unused)))
{
	file *fp = context;
	pts_pair *p = fp->f_inode->i_private;
	p->pkt_mode = buf ? (*(int *)buf != 0) : 0;
	p->pkt_status = 0;
	return 0;
}

static const command_operation pts_master_commands[256] = {
	[TIOCGPTN & 255] = { TIOCGPTN, pts_master_ioctl_tiocgptn },
	[FIONREAD & 255] = { FIONREAD, pts_master_ioctl_fionread },
	[TIOCSPTLCK & 255] = { TIOCSPTLCK, pts_master_ioctl_tiocsptlck },
	[TIOCGPTLCK & 255] = { TIOCGPTLCK, pts_master_ioctl_tiocgptlck },
	[TCFLSH & 255] = { TCFLSH, pts_master_ioctl_tcflsh },
	[TIOCPKT & 255] = { TIOCPKT, pts_master_ioctl_tiocpkt },
};

static const command_operation *const pts_master_command_groups[256] = {
	[(TIOCGPTN >> 8) & 255] = pts_master_commands,
};

int pts_master_ioctl(file *fp, unsigned cmd, void *buf)
{
	pts_pair *p = fp->f_inode->i_private;
	int ret = pts_pair_ioctl(p, cmd, buf);
	if (ret != -ENOSYS)
		return ret;
	return command_dispatch(pts_master_command_groups, fp, cmd, buf,
				-ENOSYS);
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
		if (n < 0)
			return -EINTR;
		if (nonblock && n == 0 && cyb_writer_count(p->s2m) > 0)
			return -EAGAIN;
		if (n <= 0)
			return (ssize_t)n;
		*(unsigned char *)buf = hdr;
		return (ssize_t)(n + 1);
	}

	n = cyb_getbuf(p->s2m, buf, (int)size, !nonblock, 1);
	if (n < 0)
		return -EINTR;
	if (nonblock && n == 0 && cyb_writer_count(p->s2m) > 0)
		return -EAGAIN;
	return (ssize_t)n;
}

ssize_t pts_master_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	pts_pair *p = fp->f_inode->i_private;
	const unsigned char *src = (const unsigned char *)buf;
	int nonblock = pts_file_nonblock(fp);
	int blocking = nonblock ? 0 : 1;
	size_t done = 0;
	if (!buf || size < 1)
		return 0;
	if (cyb_reader_count(p->m2s) == 0)
		return -EIO;

	while (done < size) {
		unsigned char ch;
		int ret;

		if (pts_master_consume_signal_char(p, src[done])) {
			done++;
			continue;
		}

		ch = src[done];
		ret = cyb_putbuf(p->m2s, &ch, 1, blocking, 1);
		if (ret == -EPIPE)
			return done > 0 ? (ssize_t)done : -EIO;
		if (ret < 0)
			return done > 0 ? (ssize_t)done : -EINTR;
		if (ret == 0)
			return done > 0 ? (ssize_t)done : -EAGAIN;
		done++;
	}

	return (ssize_t)done;
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
	if ((events & FS_POLL_WRITE) && !cyb_isfull(p->m2s))
		ready |= FS_POLL_WRITE;
	if (pt) {
		if (events & (FS_POLL_READ | FS_POLL_HUP))
			cyb_poll_read(p->s2m, pt);
		if (events & FS_POLL_WRITE)
			cyb_poll_write(p->m2s, pt);
	}
	return ready;
}

static int pts_slave_ioctl_tcflsh(void *context __attribute__((unused)),
				  unsigned cmd __attribute__((unused)),
				  void *buf __attribute__((unused)))
{
	file *fp = context;
	pts_pair *p = fp->f_inode->i_private;

	int sel = (int)(uintptr_t)buf;
	if (sel != TCIFLUSH && sel != TCOFLUSH && sel != TCIOFLUSH)
		sel = TCIOFLUSH;
	if (sel == TCIFLUSH || sel == TCIOFLUSH)
		cyb_flush(p->m2s);
	if (sel == TCOFLUSH || sel == TCIOFLUSH)
		cyb_flush(p->s2m);
	return 0;
}

static int pts_slave_ioctl_fionread(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf __attribute__((unused)))
{
	file *fp = context;
	pts_pair *p = fp->f_inode->i_private;
	*(int *)buf = pts_slave_fionread(p);
	return 0;
}

static int pts_slave_ioctl_kdgetmode(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	*(int *)buf = KD_TEXT;
	return 0;
}

static int pts_slave_ioctl_kdsetmode(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	return 0;
}

static int pts_slave_ioctl_gio_font(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf __attribute__((unused)))
{
	memset(buf, 0, 256 * 8);
	return 0;
}

static int pts_slave_ioctl_pio_font(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf __attribute__((unused)))
{
	return 0;
}

static int pts_slave_ioctl_gio_fontx(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	struct consolefontdesc *cfd = (struct consolefontdesc *)buf;
	if (cfd->chardata)
		memset(cfd->chardata, 0,
		       (size_t)cfd->charcount * cfd->charheight);
	cfd->charcount = 256;
	cfd->charheight = 16;
	return 0;
}

static int pts_slave_ioctl_pio_fontx(void *context __attribute__((unused)),
				     unsigned cmd __attribute__((unused)),
				     void *buf __attribute__((unused)))
{
	return 0;
}

static int pts_slave_ioctl_kdfontop(void *context __attribute__((unused)),
				    unsigned cmd __attribute__((unused)),
				    void *buf)
{
	return tty_font_ioctl(buf);
}

static int pts_slave_ioctl_pio_unimapclr(void *context __attribute__((unused)),
					 unsigned cmd __attribute__((unused)),
					 void *buf __attribute__((unused)))
{
	return 0;
}

static int pts_slave_ioctl_gio_unimap(void *context __attribute__((unused)),
				      unsigned cmd __attribute__((unused)),
				      void *buf __attribute__((unused)))
{
	struct unimapdesc *ud = (struct unimapdesc *)buf;
	ud->entry_ct = 0;
	return 0;
}

static const command_operation pts_slave_terminal_commands[256] = {
	[TCFLSH & 255] = { TCFLSH, pts_slave_ioctl_tcflsh },
	[FIONREAD & 255] = { FIONREAD, pts_slave_ioctl_fionread },
};

static const command_operation pts_slave_console_commands[256] = {
	[KDGETMODE & 255] = { KDGETMODE, pts_slave_ioctl_kdgetmode },
	[KDSETMODE & 255] = { KDSETMODE, pts_slave_ioctl_kdsetmode },
	[GIO_FONT & 255] = { GIO_FONT, pts_slave_ioctl_gio_font },
	[PIO_FONT & 255] = { PIO_FONT, pts_slave_ioctl_pio_font },
	[GIO_FONTX & 255] = { GIO_FONTX, pts_slave_ioctl_gio_fontx },
	[PIO_FONTX & 255] = { PIO_FONTX, pts_slave_ioctl_pio_fontx },
	[KDFONTOP & 255] = { KDFONTOP, pts_slave_ioctl_kdfontop },
	[PIO_UNIMAPCLR & 255] = { PIO_UNIMAPCLR,
				  pts_slave_ioctl_pio_unimapclr },
	[PIO_UNIMAP & 255] = { PIO_UNIMAP, pts_slave_ioctl_pio_unimapclr },
	[GIO_UNIMAP & 255] = { GIO_UNIMAP, pts_slave_ioctl_gio_unimap },
};

static const command_operation *const pts_slave_command_groups[256] = {
	[(TCFLSH >> 8) & 255] = pts_slave_terminal_commands,
	[(KDGETMODE >> 8) & 255] = pts_slave_console_commands,
};

int pts_slave_ioctl(file *fp, unsigned cmd, void *buf)
{
	pts_pair *p = fp->f_inode->i_private;
	int ret = pts_pair_ioctl(p, cmd, buf);
	if (ret != -ENOSYS)
		return ret;
	return command_dispatch(pts_slave_command_groups, fp, cmd, buf,
				-ENOSYS);
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
						     p->m2s, !nonblock, 1, 0,
						     NULL, NULL);
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
	return pts_slave_read_raw(p, buf, size, nonblock);
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
	if ((events & FS_POLL_WRITE) && !cyb_isfull(p->s2m))
		ready |= FS_POLL_WRITE;
	if (pt) {
		if (events & (FS_POLL_READ | FS_POLL_HUP))
			cyb_poll_read(p->m2s, pt);
		if (events & FS_POLL_WRITE)
			cyb_poll_write(p->s2m, pt);
	}
	return ready;
}
