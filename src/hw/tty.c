
/*
 * src/hw/tty.c - TTY driver: VGA/framebuffer primitives, cursor management,
 * ANSI escape processing, and high-level text output.
 */

#include <int/int.h>
#include <hw/tty.h>
#include <hw/vga.h>
#include <fs/ioctl.h>
#include <lib/port.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <unistd.h>
#include <errno.h>

/* ── VGA text-mode video buffer ──────────────────────────────────────────── */

static char *vidptr = (char *)0xC00b8000;

unsigned TTY_MAX_ROW;
unsigned TTY_MAX_COL;

/* ── Cursor state ────────────────────────────────────────────────────────── */

spinlock_t tty_lock;

static int cursor;

/* ── Terminal settings / foreground process group ────────────────────────── */

static struct termios tty_termios = {
	.c_iflag = ICRNL,
	.c_oflag = OPOST | ONLCR,
	.c_cflag = B38400 | CS8,
	.c_lflag = IXON | ISIG | ICANON | ECHO | ECHOE | ECHOCTL | ECHOKE,
	.c_line = 0,
	.c_cc = INIT_C_CC,
};

static unsigned tty_pgrp;

struct termios *tty_gettermios()
{
	return &tty_termios;
}

#define CUR_ROW (cursor / (int)TTY_MAX_COL)
#define CUR_COL (cursor % (int)TTY_MAX_COL)

/* ── ANSI escape state ───────────────────────────────────────────────────── */

static int ansi_flag;
static char ansi_buf[10];
static int ansi_idx;

static void ansi_begin(void)
{
	memset(ansi_buf, 0, sizeof(ansi_buf));
	ansi_idx = 0;
	ansi_flag = 1;
}

static void ansi_end(void)
{
	memset(ansi_buf, 0, sizeof(ansi_buf));
	ansi_idx = 0;
	ansi_flag = 0;
}

/* Forward declaration needed by ansi_feed. */
static void tty_cursor_set(int pos);

static void ansi_feed(char c)
{
	char *arg = ansi_buf + 1;
	int val, row, col;

	switch (c) {
	case 'm': /* SGR - colour/attribute (stub) */
		ansi_end();
		return;
	case 'A': /* cursor up n */
		val = atoi(arg);
		row = CUR_ROW - val;
		if (row < 0)
			row = 0;
		tty_cursor_set(ROW_COL_TO_CUR(row, CUR_COL));
		ansi_end();
		return;
	case 'B': /* cursor down n */
		val = atoi(arg);
		row = CUR_ROW + val;
		if (row >= (int)TTY_MAX_ROW)
			row = TTY_MAX_ROW - 1;
		tty_cursor_set(ROW_COL_TO_CUR(row, CUR_COL));
		ansi_end();
		return;
	case 'C': /* cursor right n */
		val = atoi(arg);
		col = CUR_COL + val;
		if (col >= (int)TTY_MAX_COL)
			col = TTY_MAX_COL - 1;
		tty_cursor_set(ROW_COL_TO_CUR(CUR_ROW, col));
		ansi_end();
		return;
	case 'D': /* cursor left n */
		val = atoi(arg);
		col = CUR_COL - val;
		if (col < 0)
			col = 0;
		tty_cursor_set(ROW_COL_TO_CUR(CUR_ROW, col));
		ansi_end();
		return;
	case 'H': { /* set cursor position row;col (1-based) */
		char *str_col = strchr(arg, ';');

		if (!str_col)
			break;
		*str_col++ = '\0';
		row = atoi(arg) - 1;
		col = atoi(str_col) - 1;
		if (row < 0)
			row = 0;
		if (row >= (int)TTY_MAX_ROW)
			row = TTY_MAX_ROW - 1;
		if (col < 0)
			col = 0;
		if (col >= (int)TTY_MAX_COL)
			col = TTY_MAX_COL - 1;
		tty_cursor_set(ROW_COL_TO_CUR(row, col));
		ansi_end();
		return;
	}
	case 'J': /* erase display */
		tty_clear();
		ansi_end();
		return;
	default:
		break;
	}

	/* Accumulate non-terminator characters; end on any letter. */
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
		ansi_end();
	} else if (ansi_idx < (int)sizeof(ansi_buf) - 1) {
		ansi_buf[ansi_idx++] = c;
	}
}

/* ── VGA/FB primitives ───────────────────────────────────────────────────── */

static void tty_clear_row(int row)
{
	int col;

	for (col = 0; col < (int)TTY_MAX_COL; col++)
		tty_putchar(row, col, ' ');
}

void tty_init(void)
{
	int i, j;

	if (fb_is_available()) {
		fb_get_char_dims(&TTY_MAX_COL, &TTY_MAX_ROW);
	} else {
		TTY_MAX_ROW = 25;
		TTY_MAX_COL = 80;
	}

	for (i = 0; i < (int)TTY_MAX_ROW; i++)
		for (j = 0; j < (int)TTY_MAX_COL; j++)
			tty_putchar(i, j, ' ');

	cursor = 0;
	spinlock_init(&tty_lock);
}

void tty_setcolor(int x, int y, TTY_COLOR front, TTY_COLOR back)
{
	int color_field = (back << 4) | front;
	int cur = ROW_COL_TO_CUR(x, y);

	if (x < 0 || x >= (int)TTY_MAX_ROW || y < 0 || y >= (int)TTY_MAX_COL)
		return;

	if (!fb_is_available()) {
		vidptr[cur * 2 + 1] = (char)color_field;
	} else {
		/* FIXME: only solid colour supported in FB text mode */
		fb_write_color(y, x, VGA_COLOR_BLACK);
	}
}

TTY_COLOR tty_get_frontcolor(int x, int y)
{
	int cur = ROW_COL_TO_CUR(x, y);

	if (x < 0 || x >= (int)TTY_MAX_ROW || y < 0 || y >= (int)TTY_MAX_COL)
		return clBlack;

	if (!fb_is_available())
		return (TTY_COLOR)((unsigned char)vidptr[cur * 2 + 1] & 0x0f);
	return clWhite;
}

TTY_COLOR tty_get_backcolor(int x, int y)
{
	int cur = ROW_COL_TO_CUR(x, y);

	if (x < 0 || x >= (int)TTY_MAX_ROW || y < 0 || y >= (int)TTY_MAX_COL)
		return clBlack;

	if (!fb_is_available())
		return (TTY_COLOR)(((unsigned char)vidptr[cur * 2 + 1] >> 4) &
				   0x07);
	return clBlack;
}

void tty_putchar(int x, int y, char c)
{
	if (x < 0 || x >= (int)TTY_MAX_ROW || y < 0 || y >= (int)TTY_MAX_COL)
		return;

	if (!fb_is_available()) {
		int cur = ROW_COL_TO_CUR(x, y);
		vidptr[cur * 2] = c;
	} else {
		fb_putchar(y, x, c);
	}
}

char tty_getchar(int x, int y)
{
	if (x < 0 || x >= (int)TTY_MAX_ROW || y < 0 || y >= (int)TTY_MAX_COL)
		return ' ';

	if (!fb_is_available()) {
		int cur = ROW_COL_TO_CUR(x, y);
		return vidptr[cur * 2];
	}
	return fb_getchar(y, x);
}

void tty_roll_one_line(void)
{
	if (!fb_is_available()) {
		char *dst = vidptr;
		char *src = dst + TTY_MAX_COL * 2;
		memmove(dst, src, TTY_MAX_COL * (TTY_MAX_ROW - 1) * 2);
		tty_clear_row(TTY_MAX_ROW - 1);
	} else {
		fb_scroll_line();
	}
}

void tty_clear(void)
{
	if (!fb_is_available()) {
		int row;
		char *src = vidptr;
		unsigned len = TTY_MAX_COL * 2;

		tty_clear_row(0);
		for (row = 1; row < (int)TTY_MAX_ROW; row++)
			memcpy(src + row * len, src, len);
	} else {
		fb_clear_screen();
	}
}

void tty_movecurse(unsigned c)
{
	if (!fb_is_available()) {
		unsigned short cp = (unsigned short)c;
		port_write_word(0x3d4, 0x0e | (cp & 0xff00));
		port_write_word(0x3d4, 0x0f | (cp << 8));
	} else {
		fb_update_cursor(c);
	}
}

/* ── Cursor helpers ──────────────────────────────────────────────────────── */

/*
 * tty_cursor_set - set cursor position and scroll if past end of screen.
 * Does NOT update the hardware cursor register.
 */
static void tty_cursor_set(int pos)
{
	if (pos < 0)
		return;
	cursor = pos;
	while (cursor >= (int)TTY_MAX_CHARS) {
		tty_roll_one_line();
		cursor -= TTY_MAX_COL;
	}
}

/*
 * tty_cursor_forward - set cursor and update the hardware cursor register.
 */
static void tty_cursor_forward(int pos)
{
	tty_cursor_set(pos);
	tty_movecurse((unsigned)cursor);
}

/* ── Internal character rendering ────────────────────────────────────────── */

/*
 * tty_char_to_pos - process one character and return the new cursor position.
 * Returns -1 if the character is unrecognised. Does NOT update cursor or
 * the hardware register; the caller decides when to flush.
 */
static int tty_char_to_pos(char c)
{
	int i, pos, spaces;

	if (ansi_flag) {
		ansi_feed(c);
		return cursor;
	}

	switch ((unsigned char)c) {
	case '\n':
		return ROW_COL_TO_CUR(CUR_ROW + 1, CUR_COL);
	case '\r':
		return ROW_COL_TO_CUR(CUR_ROW, 0);
	case '\b':
		return cursor > 0 ? cursor - 1 : 0;
	case '\t':
		spaces = 8 - (CUR_COL % 8);
		pos = cursor;
		for (i = 0; i < spaces; i++) {
			tty_putchar(pos / TTY_MAX_COL, pos % TTY_MAX_COL, ' ');
			pos++;
		}
		return pos;
	case 0x0c: /* form feed: clear screen */
		tty_clear();
		return cursor;
	case 0x1b: /* ESC: begin ANSI sequence */
		ansi_begin();
		return cursor;
	default:
		if (isprint(c)) {
			tty_putchar(CUR_ROW, CUR_COL, c);
			return cursor + 1;
		}
		return -1;
	}
}

/* ── Public text output ──────────────────────────────────────────────────── */

/*
 * tty_emit - emit one character, updating cursor and hardware register.
 * ctx is ignored; present for compatibility with the fputstr callback type.
 */
void tty_emit(char c, void *ctx)
{
	(void)ctx;
	tty_cursor_forward(tty_char_to_pos(c));
}

void tty_set_cursor(int pos)
{
	if (pos >= 0)
		cursor = pos;
}

void tty_flush_cursor(void)
{
	tty_cursor_forward(cursor);
}

int tty_get_cursor(void)
{
	return cursor;
}

void tty_clear_locked(void)
{
	spinlock_lock(&tty_lock);
	tty_clear();
	cursor = 0;
	spinlock_unlock(&tty_lock);
}

/*
 * tty_raw_write - write raw bytes to the TTY under the TTY lock.
 * Batches hardware cursor updates: the hardware register is written only once
 * at the end, not per character.
 */
/* Returns 1 if the cursor is currently at column 0. */

static void tty_raw_write(const char *buf, unsigned len)
{
	unsigned i;

	spinlock_lock(&tty_lock);
	for (i = 0; i < len; i++)
		tty_cursor_set(tty_char_to_pos(buf[i]));
	tty_movecurse((unsigned)cursor);
	spinlock_unlock(&tty_lock);
}

static int at_column_zero(void)
{
	return tty_get_cursor() % (int)TTY_MAX_COL == 0;
}

/* Apply c_oflag output processing and write one character to the TTY.
 * If OPOST is clear, the character is written raw. */
static void output_char(unsigned char c)
{
	if (!(tty_termios.c_oflag & OPOST)) {
		tty_raw_write((const char *)&c, 1);
		return;
	}

	if (tty_termios.c_oflag & OLCUC)
		c = (unsigned char)toupper(c);

	if (c == '\n') {
		if (tty_termios.c_oflag & ONLCR) {
			tty_raw_write("\r\n", 2);
			return;
		}
	} else if (c == '\r') {
		if (tty_termios.c_oflag & OCRNL) {
			tty_raw_write("\n", 1);
			return;
		}
		if ((tty_termios.c_oflag & ONOCR) && at_column_zero())
			return;
	}

	tty_raw_write((const char *)&c, 1);
}

void tty_write(const char *buf, unsigned len)
{
	int i;
	for (i = 0; i < len; i++)
		output_char(buf[i]);
}

/* ── ioctl ───────────────────────────────────────────────────────────────── */

int tty_ioctl(file *file, unsigned cmd, void *buf)
{
	switch (cmd) {
	case TCGETS: {
		memcpy(buf, &tty_termios, sizeof(tty_termios));
		return 0;
	}
	case TCSETS:
	case TCSETSW:
		memcpy(&tty_termios, buf, sizeof(tty_termios));
		return 0;
	case TIOCGWINSZ: {
		struct winsize *size = (struct winsize *)buf;
		size->ws_row = TTY_MAX_ROW;
		size->ws_col = TTY_MAX_COL;
		size->ws_xpixel = size->ws_ypixel = 0;
		return 0;
	}
	case TIOCGPGRP:
		*(unsigned *)buf = tty_pgrp;
		return 0;
	case TIOCSPGRP:
		tty_pgrp = *(unsigned *)buf;
		return 0;
	}
	return -ENOSYS;
}
