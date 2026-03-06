
/*
 * Copyright (C) 2014  Ender Zheng
 * License: GPL version 2 or higher http://www.gnu.org/licenses/gpl.html
 */
#include <int.h>
#include <tty.h>
#include <vga.h>
#include <unistd.h>
#include <ioctl.h>
#include <port.h>

/* VGA text-mode video buffer (BIOS address, used only when FB is absent). */
static char *vidptr = (char *)0xC00b8000;

unsigned TTY_MAX_ROW;
unsigned TTY_MAX_COL;

static void tty_clear_row(int row);

void tty_init(void)
{
	int i, j;

	if (fb_is_available()) {
		fb_get_char_dims(&TTY_MAX_COL, &TTY_MAX_ROW);
	} else {
		TTY_MAX_ROW = 25;
		TTY_MAX_COL = 80;
	}

	for (i = 0; i < TTY_MAX_ROW; i++)
		for (j = 0; j < TTY_MAX_COL; j++)
			tty_putchar(i, j, ' ');
}

void tty_setcolor(int x, int y, TTY_COLOR front, TTY_COLOR back)
{
	int color_field = (back << 4) | front;
	int cur = ROW_COL_TO_CUR(x, y);

	if (x < 0 || x >= TTY_MAX_ROW || y < 0 || y >= TTY_MAX_COL)
		return;

	if (!fb_is_available()) {
		vidptr[cur * 2 + 1] = color_field;
	} else {
		/* FIXME: only solid color supported in FB text mode */
		fb_write_color(y, x, VGA_COLOR_BLACK);
	}
}

TTY_COLOR tty_get_frontcolor(int x, int y)
{
	int cur = ROW_COL_TO_CUR(x, y);

	if (x < 0 || x >= TTY_MAX_ROW || y < 0 || y >= TTY_MAX_COL)
		return clBlack;

	if (!fb_is_available()) {
		return vidptr[cur * 2 + 1] % 8;
	} else {
		/* FIXME: only white supported in FB text mode */
		return clWhite;
	}
}

TTY_COLOR tty_get_backcolor(int x, int y)
{
	int cur = ROW_COL_TO_CUR(x, y);

	if (x < 0 || x >= TTY_MAX_ROW || y < 0 || y >= TTY_MAX_COL)
		return clBlack;

	if (!fb_is_available()) {
		return vidptr[cur * 2 + 1] >> 4;
	} else {
		/* FIXME: only black supported in FB text mode */
		return clBlack;
	}
}

void tty_putchar(int x, int y, char c)
{
	if (x < 0 || x >= TTY_MAX_ROW || y < 0 || y >= TTY_MAX_COL)
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
	if (x < 0 || x >= TTY_MAX_ROW || y < 0 || y >= TTY_MAX_COL)
		return ' ';

	if (!fb_is_available()) {
		int cur = ROW_COL_TO_CUR(x, y);
		return vidptr[cur * 2];
	} else {
		return fb_getchar(y, x);
	}
}

#define CUR_ROW (cursor / TTY_MAX_COL)
#define CUR_COL (cursor % TTY_MAX_COL)

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

static void tty_clear_row(int row)
{
	int col;
	for (col = 0; col < TTY_MAX_COL; col++)
		tty_putchar(row, col, ' ');
}

void tty_clear(void)
{
	if (!fb_is_available()) {
		int row;
		char *src = vidptr;
		unsigned len = TTY_MAX_COL * 2;
		tty_clear_row(0);
		for (row = 1; row < TTY_MAX_ROW; row++)
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

int tty_ioctl(file *file, unsigned cmd, void *buf)
{
	switch (cmd) {
	case TCGETS: {
		struct termios s = {
			ICRNL, /* change incoming CR to NL */
			OPOST | ONLCR, /* change outgoing NL to CRNL */
			B38400 | CS8,
			IXON | ISIG | ICANON | ECHO | ECHOCTL | ECHOKE,
			0, /* console termio */
			INIT_C_CC
		};
		memcpy(buf, &s, sizeof(s));
		return 0;
	}
	case TCSETS: {
		/* FIXME */
		return 0;
	}
	case TIOCGWINSZ: {
		struct winsize *size = (struct winsize *)buf;
		size->ws_row = TTY_MAX_ROW;
		size->ws_col = TTY_MAX_COL;
		size->ws_xpixel = size->ws_ypixel = 0;
		return 0;
	}
	}
	return -1;
}
