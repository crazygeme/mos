#ifndef _HW_TTY_H_
#define _HW_TTY_H_

#include <mm/mm.h>
#include <fs/fs.h>
#include <lib/lock.h>

typedef enum _TTY_COLOR {
	clBlack,
	clBlue,
	clGreen,
	clCyan,
	clRed,
	clMagenta,
	clBrown,
	clLightGray,
	clDarkGray,
	clLightBlue,
	clLightGreen,
	clLightCyan,
	clLightRed,
	clLightMagenta,
	clYellow,
	clWhite
} TTY_COLOR;

extern unsigned TTY_MAX_ROW;
extern unsigned TTY_MAX_COL;
#define TTY_MAX_CHARS (TTY_MAX_ROW * TTY_MAX_COL)
#define ROW_COL_TO_CUR(row, col) ((row) * (int)TTY_MAX_COL + (col))

extern spinlock_t tty_lock;

/* Lifecycle */
void tty_init(void);

/* Low-level cell access */
void tty_setcolor(int x, int y, TTY_COLOR front, TTY_COLOR back);
TTY_COLOR tty_get_frontcolor(int x, int y);
TTY_COLOR tty_get_backcolor(int x, int y);
void tty_putchar(int x, int y, char c);
char tty_getchar(int x, int y);

/* Screen operations */
void tty_roll_one_line(void);
void tty_clear(void);
void tty_movecurse(unsigned cp);

/* Cursor management */
void tty_set_cursor(int pos);
void tty_flush_cursor(void);
int tty_get_cursor(void);
void tty_clear_locked(void);

/* Text output */
void tty_emit(char c, void *ctx);
void tty_print(char *str, void *ctx);
void tty_write(const char *buf, unsigned len);

/* ioctl */
int tty_ioctl(file *file, unsigned cmd, void *buf);

struct termios *tty_gettermios();

#endif
