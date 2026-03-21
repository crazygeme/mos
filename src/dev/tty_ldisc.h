/*
 * tty_ldisc.h — shared TTY/PTY line discipline interface.
 *
 * Included by tty.c (virtual consoles) and pts.c (pseudo-terminals).
 */
#ifndef _FS_TTY_LDISC_H_
#define _FS_TTY_LDISC_H_

#include <fs/ioctl.h> /* struct termios, tcflag_t */
#include <lib/cyclebuf.h> /* cy_buf */

/* ── Canonical-mode line buffer ──────────────────────────────────────────── */

#define TTY_CANON_BUF_SIZE 256

typedef struct {
	char buf[TTY_CANON_BUF_SIZE];
	int len;
} tty_canon_t;

/* ── Default terminal settings ───────────────────────────────────────────── */

extern const struct termios tty_default_termios;

/* ── Echo callback ───────────────────────────────────────────────────────── */

typedef void (*tty_ldisc_echo_fn)(void *ctx, const char *buf, int len);

/* ── Public functions ────────────────────────────────────────────────────── */

/*
 * tty_input_translate - apply c_iflag transformations to one input byte.
 * Returns the (possibly modified) byte, or -1 to discard (IGNCR on CR).
 */
int tty_input_translate(unsigned char c, tcflag_t iflag);

/*
 * tty_canon_drain - copy up to size bytes out of canon into dst,
 * sliding remaining bytes to the front.  Returns bytes copied.
 */
int tty_canon_drain(tty_canon_t *canon, char *dst, int size);

/*
 * tty_ldisc_canon_readline - read one canonical line from buf into canon.
 *
 * Returns 1 if a complete line is ready, 0 on EOF, -1 if interrupted by a
 * signal (EINTR).  Set check_eof when the source signals master-closed via
 * the 0xFF sentinel (pty slave); clear it for keyboard input.
 */
int tty_ldisc_canon_readline(tty_canon_t *canon, const struct termios *tc,
			     cy_buf *buf, int check_eof, unsigned pgrp,
			     tty_ldisc_echo_fn echo, void *ctx);

#endif /* _FS_TTY_LDISC_H_ */
