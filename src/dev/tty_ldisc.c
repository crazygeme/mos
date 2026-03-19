/*
 * tty_ldisc.c — TTY/PTY line discipline implementation.
 */

#include <fs/ioctl.h>
#include <lib/klib.h> /* memcpy, memmove, tolower */
#include <lib/cyclebuf.h>
#include <ps/ps.h> /* ps_send_signal_pgrp */
#include "tty_ldisc.h"

/* ── Default termios ─────────────────────────────────────────────────────── */

const struct termios tty_default_termios = {
	.c_iflag = ICRNL,
	.c_oflag = OPOST | ONLCR,
	.c_cflag = B38400 | CS8,
	.c_lflag = IXON | ISIG | ICANON | ECHO | ECHOE | ECHOCTL | ECHOKE,
	.c_line = 0,
	.c_cc = INIT_C_CC,
};

/* ── Public: input flag transformation ───────────────────────────────────── */

int tty_input_translate(unsigned char c, tcflag_t iflag)
{
	if (iflag & ISTRIP)
		c &= 0x7f;
	if (c == '\r') {
		if (iflag & IGNCR)
			return -1;
		if (iflag & ICRNL)
			return '\n';
	} else if (c == '\n' && (iflag & INLCR)) {
		return '\r';
	}
	if (iflag & IUCLC)
		c = (unsigned char)tolower(c);
	return (int)c;
}

/* ── Public: canon buffer drain ──────────────────────────────────────────── */

int tty_canon_drain(tty_canon_t *canon, char *dst, int size)
{
	int n = size < canon->len ? size : canon->len;
	memcpy(dst, canon->buf, (unsigned)n);
	canon->len -= n;
	if (canon->len > 0)
		memmove(canon->buf, canon->buf + n, (unsigned)canon->len);
	return n;
}

/* ── Private: per-character helpers ─────────────────────────────────────── */

static int isig_char(const struct termios *tc, unsigned char c)
{
	if (!(tc->c_lflag & ISIG))
		return 0;
	if (tc->c_cc[VINTR] && c == tc->c_cc[VINTR])
		return SIGINT;
	if (tc->c_cc[VQUIT] && c == tc->c_cc[VQUIT])
		return SIGQUIT;
	if (tc->c_cc[VSUSP] && c == tc->c_cc[VSUSP])
		return SIGTSTP;
	return 0;
}

static void canon_erase(tty_canon_t *canon, const struct termios *tc,
			tty_ldisc_echo_fn echo, void *ctx)
{
	if (canon->len == 0)
		return;
	canon->len--;
	if (!echo || !(tc->c_lflag & ECHO))
		return;
	if (tc->c_lflag & ECHOE)
		echo(ctx, "\b \b", 3);
	else
		echo(ctx, (const char *)&tc->c_cc[VERASE], 1);
}

static void canon_kill(tty_canon_t *canon, const struct termios *tc,
		       tty_ldisc_echo_fn echo, void *ctx)
{
	if (echo && (tc->c_lflag & ECHO)) {
		if (tc->c_lflag & ECHOK) {
			int i;
			for (i = 0; i < canon->len; i++)
				echo(ctx, "\b \b", 3);
		} else {
			echo(ctx, (const char *)&tc->c_cc[VKILL], 1);
			echo(ctx, "\n", 1);
		}
	}
	canon->len = 0;
}

static void canon_append(tty_canon_t *canon, const struct termios *tc,
			 unsigned char c, tty_ldisc_echo_fn echo, void *ctx)
{
	if (canon->len >= TTY_CANON_BUF_SIZE - 1)
		return;
	canon->buf[canon->len++] = (char)c;
	if (!echo)
		return;
	if (tc->c_lflag & ECHO)
		echo(ctx, &canon->buf[canon->len - 1], 1);
	else if (c == '\n' && (tc->c_lflag & ECHONL))
		echo(ctx, "\n", 1);
}

/* ── Public: canonical readline ─────────────────────────────────────────── */

int tty_ldisc_canon_readline(tty_canon_t *canon, const struct termios *tc,
			     cy_buf *buf, int check_eof, unsigned pgrp,
			     tty_ldisc_echo_fn echo, void *ctx)
{
	while (1) {
		unsigned char raw = cyb_getc(buf);
		if (check_eof && raw == (unsigned char)EOF)
			return canon->len > 0 ? 1 : 0;

		int ch = tty_input_translate(raw, tc->c_iflag);
		if (ch < 0)
			continue;
		unsigned char c = (unsigned char)ch;

		int sig = isig_char(tc, c);
		if (sig) {
			canon->len = 0;
			ps_send_signal_pgrp(pgrp, sig);
			return 0;
		}
		if (tc->c_cc[VEOF] && c == tc->c_cc[VEOF])
			return canon->len > 0 ? 1 : 0;
		if (tc->c_cc[VERASE] && c == tc->c_cc[VERASE]) {
			canon_erase(canon, tc, echo, ctx);
			continue;
		}
		if (tc->c_cc[VKILL] && c == tc->c_cc[VKILL]) {
			canon_kill(canon, tc, echo, ctx);
			continue;
		}
		canon_append(canon, tc, c, echo, ctx);
		if (c == '\n' || (tc->c_cc[VEOL] && c == tc->c_cc[VEOL]) ||
		    (tc->c_cc[VEOL2] && c == tc->c_cc[VEOL2]))
			return 1;
	}
}
