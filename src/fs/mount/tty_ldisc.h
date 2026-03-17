/*
 * include/fs/tty_ldisc.h — shared TTY/PTY line discipline helpers
 *
 * Included by both tty.c (virtual consoles) and pts.c (pseudo-terminals).
 * Provides:
 *   - tty_canon_t         canonical-mode line buffer
 *   - tty_default_termios default terminal settings
 *   - tty_input_translate  c_iflag byte transformation
 *   - tty_is_eol           end-of-line detection
 *   - tty_canon_drain      drain bytes from the line buffer
 */
#ifndef _FS_TTY_LDISC_H_
#define _FS_TTY_LDISC_H_

#include <fs/ioctl.h>   /* struct termios, tcflag_t, ICRNL, VEOL, … */
#include <lib/klib.h>   /* memcpy, memmove, tolower */

/* ── Canonical-mode accumulation buffer ──────────────────────────────────── */

#define TTY_CANON_BUF_SIZE 256

typedef struct {
	char buf[TTY_CANON_BUF_SIZE];
	int  len;
} tty_canon_t;

/* ── Shared default termios ───────────────────────────────────────────────── */

/*
 * tty_default_termios - canonical mode, echo, ONLCR output, 38400-8N1.
 * Declared static const so each translation unit gets its own copy;
 * the compiler/linker will fold identical read-only sections.
 */
static const struct termios tty_default_termios = {
	.c_iflag = ICRNL,
	.c_oflag = OPOST | ONLCR,
	.c_cflag = B38400 | CS8,
	.c_lflag = IXON | ISIG | ICANON | ECHO | ECHOE | ECHOCTL | ECHOKE,
	.c_line  = 0,
	.c_cc    = INIT_C_CC,
};

/* ── Input flag transformation ────────────────────────────────────────────── */

/*
 * tty_input_translate - apply c_iflag rules to one input byte.
 * Returns the (possibly modified) byte, or -1 if the byte should be
 * silently discarded (IGNCR on a CR).
 */
static inline int tty_input_translate(unsigned char c, tcflag_t iflag)
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

/* ── End-of-line detection ────────────────────────────────────────────────── */

/*
 * tty_is_eol - return 1 if c terminates a canonical input line.
 */
static inline int tty_is_eol(unsigned char c, const struct termios *tc)
{
	return c == '\n' ||
	       (tc->c_cc[VEOL]  && c == tc->c_cc[VEOL])  ||
	       (tc->c_cc[VEOL2] && c == tc->c_cc[VEOL2]);
}

/* ── Canonical drain ──────────────────────────────────────────────────────── */

/*
 * tty_canon_drain - copy up to size bytes from canon->buf into dst,
 * then shift the remaining unread bytes to the front of the buffer.
 * Returns the number of bytes copied.
 */
static inline int tty_canon_drain(tty_canon_t *canon, char *dst, int size)
{
	int n = size < canon->len ? size : canon->len;
	memcpy(dst, canon->buf, (unsigned)n);
	canon->len -= n;
	if (canon->len > 0)
		memmove(canon->buf, canon->buf + n, (unsigned)canon->len);
	return n;
}

#endif /* _FS_TTY_LDISC_H_ */
