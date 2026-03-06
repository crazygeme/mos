
/*
 * Copyright (C) 2014  Ender Zheng
 * License: GPL version 2 or higher http://www.gnu.org/licenses/gpl.html
 */
#include "klib.h"
#include <mount.h>
#include <ps.h>
#include <time.h>
#include <int.h>
#include <tty.h>
#include <vga.h>
#include <unistd.h>
#include <ioctl.h>
#include <port.h>
#include <macro.h>

spinlock_t tty_lock;

/* VGA text-mode video buffer (BIOS address, used only when FB is absent). */
static char *vidptr = (char *)0xC00b8000;

unsigned TTY_MAX_ROW;
unsigned TTY_MAX_COL;

static void tty_clear_row(int row);

void tty_init(void)
{
	int i, j;

	spinlock_init(&tty_lock);

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
	spinlock_lock(&tty_lock);

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

	spinlock_unlock(&tty_lock);
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

void tty_raw_write(const char *buf, unsigned len)
{
	unsigned i;

	spinlock_lock(&tty_lock);
	for (i = 0; i < len; i++) {
		if (klib_get_pos() >= TTY_MAX_CHARS)
			klib_flush_cursor();
		klib_update_cursor(klib_putchar(buf[i], NULL));
	}
	klib_flush_cursor();
	spinlock_unlock(&tty_lock);
}

static ssize_t tty_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	if (size < 1 || !buf)
		return 0;
	tty_raw_write(buf, size);
	return (ssize_t)size;
}

static ssize_t tty_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	memset(buf, '?', size);
	return (ssize_t)size;
}

static int tty_release(inode *node, file *fp)
{
	free(node);
	return 0;
}

static loff_t tty_llseek(file *fp, loff_t offset, int whence)
{
	switch (whence) {
	case SEEK_SET:
		klib_update_cursor(offset);
		break;
	case SEEK_CUR:
		klib_update_cursor(offset + klib_get_pos());
		break;
	case SEEK_END:
		klib_update_cursor(TTY_MAX_CHARS - offset);
		break;
	default:
		break;
	}
	klib_flush_cursor();
	fp->f_pos = klib_get_pos();
	return fp->f_pos;
}

static int tty_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT || type == FS_POLL_READ)
		return -1;
	/* can always write */
	return 0;
}

static int tty_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR);
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0xb;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_nlink = 1;
	s->st_rdev = 8004;
	s->st_size = TTY_MAX_CHARS - klib_get_pos();
	return 0;
}

static const inode_operations tty_iops = {
	.getattr = tty_getattr,
};

static const file_operations tty_fops = {
	.release = tty_release,
	.read = tty_read,
	.write = tty_write,
	.llseek = tty_llseek,
	.poll = tty_poll,
	.ioctl = tty_ioctl,
};

static inode *tty_get_root(super_block *sb)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFCHR;
	node->i_op = &tty_iops;
	node->i_fop = &tty_fops;
	return node;
}

static super_operations tty_sops = {
	.get_root = tty_get_root,
};

static void tty_setup()
{
	task_struct *cur = CURRENT_TASK();
	vfs_mount(cur->root, "/dev/tty", &tty_sops);
}

KERNEL_INIT(4, tty_setup);