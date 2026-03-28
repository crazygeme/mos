/*
 * src/fs/mount/tty.c - TTY driver: VGA/framebuffer output, keyboard input,
 * ANSI escape processing, termios, and /dev/tty filesystem device.
 *
 * Supports up to 10 virtual terminals (TTY 0-9).  The active TTY owns the
 * real framebuffer; inactive TTYs cache output in a per-TTY text buffer that
 * is restored to the screen on switch-back.  Each TTY has its own keyboard
 * input queue; only the active TTY receives keystrokes.
 *
 * TTY switching is triggered by the keyboard driver calling tty_switch(n)
 * (bound to Ctrl+Alt+F1..F10).  If TTY n has no live process, /bin/bash is
 * spawned with its stdio wired to /dev/ttyn.
 *
 * Public kernel interface (see include/hw/tty.h):
 *   tty_init()                    - early VGA/FB init, called before the VM
 *   tty_default_emit_unsafe(c,ctx)- single-char output callback for kprint
 *   tty_lock_acquire()            - acquire the active TTY spinlock
 *   tty_lock_release()            - release the active TTY spinlock
 *   tty_default_clear()           - clear screen under the TTY lock
 *   tty_switch(n)                 - switch active terminal to index n
 *   tty_active_kb_put(c)          - enqueue a byte into the active TTY's kb buf
 */

#include "config.h"
#include <int/int.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/vfs.h>
#include <fs/ioctl.h>
#include <hw/tty.h>
#include <hw/vga.h>
#include <hw/time.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <ps/ps.h>
#include <elf/exec.h>
#include <unistd.h>
#include <errno.h>
#include <macro.h>
#include <dev/dev.h>
#include <ext4_oflags.h>
#include "tty_ldisc.h"
#include <hw/keyboard.h>

#define TTY_SWITCH_COUNT 10 /* how many TTYs support switching (0-9) */

/* ── Private TTY state ───────────────────────────────────────────────────── */

typedef struct {
	/* framebuffer dimensions */
	unsigned max_row, max_col;
	/* output lock — also protects cursor */
	spinlock_t lock;
	/* cursor position (character index from top-left) */
	int cursor;
	/* terminal settings */
	struct termios termios;
	unsigned pgrp;
	/* ANSI escape sequence parser */
	int ansi_flag;
	char ansi_buf[10];
	int ansi_idx;
	/* canonical input line buffer */
	tty_canon_t canon;
	/* per-TTY keyboard input queue */
	cy_buf *kb_buf;
	/* screen text buffer for inactive TTYs (also used on switch-away save) */
	char *saved_text;
	/* per-cell fg/bg color arrays matching saved_text */
	unsigned *saved_fg;
	unsigned *saved_bg;
	/* TTY index (0..TTY_MAX_VDEV-1) */
	int tty_idx;
	/* PID of the bash process running on this TTY (0 = none) */
	unsigned bash_pid;
	/* Parent task for this TTY, used for setting root and waitpid */
	task_struct *parent;
	/* number of open file structs referencing this TTY */
	int open_count;
	/* set when last fd is closed; guards read/write against stale access */
	int released;
	int saved_cursor;
	int scroll_top;
	int scroll_bot;
	/* DEC private mode flags */
	int cursor_hidden; /* ?25l: do not draw hardware cursor */
	int no_wrap; /* ?7l: disable auto-wrap at line end */
	/* alternate screen buffer for ?1049h/l */
	char *alt_text;
	int alt_cursor;
	/* current foreground/background colors (set by SGR escape sequences) */
	unsigned fg_color;
	unsigned bg_color;
	/* keyboard translation mode (K_XLATE, K_RAW, etc.) */
	int kb_mode;
} tty_state;

#define DEFAULT_TTY 0
static tty_state ttys[TTY_MAX_VDEV];
static int active_tty_idx = DEFAULT_TTY;
static tty_state *this_ttys = &ttys[DEFAULT_TTY];

/*
 * tty_switch_lock - global spinlock protecting this_ttys, active_tty_idx,
 * and the framebuffer save/restore during a TTY switch.  Also acquired by
 * tty_active_kb_put() so keyboard input is always routed to the correct TTY
 * even when a switch is in progress on another CPU.
 */
static spinlock_t tty_switch_lock;

/* ── Convenience macros ──────────────────────────────────────────────────── */

#define MAX_ROW (state->max_row)
#define MAX_COL (state->max_col)
#define MAX_CHARS ((int)(MAX_ROW * MAX_COL))
#define CUR_ROW (state->cursor / (int)MAX_COL)
#define CUR_COL (state->cursor % (int)MAX_COL)

/* ── VGA/framebuffer primitives ──────────────────────────────────────────── */

/*
 * vga_putchar - write character to either the live framebuffer (active TTY)
 * or the TTY's saved_text shadow (inactive TTY).
 */
static void vga_putchar(tty_state *state, int x, int y, char c)
{
	if (x < 0 || x >= (int)MAX_ROW || y < 0 || y >= (int)MAX_COL)
		return;
	if (state->tty_idx == active_tty_idx) {
		fb_putchar(y, x, c, state->fg_color, state->bg_color);
	} else {
		/* Inactive TTY: write to saved_text and color arrays */
		int idx = x * (int)MAX_COL + y;
		state->saved_text[idx] = c;
		state->saved_fg[idx] = state->fg_color;
		state->saved_bg[idx] = state->bg_color;
	}
}

static void tty_do_clear(tty_state *state)
{
	if (state->tty_idx == active_tty_idx) {
		fb_clear_screen();
	} else {
		unsigned sz = (unsigned)(MAX_ROW * (int)MAX_COL);
		memset(state->saved_text, 0, sz);
		memset(state->saved_fg, 0xFF, sz * sizeof(unsigned));
		memset(state->saved_bg, 0, sz * sizeof(unsigned));
	}
}

static void tty_roll_line(tty_state *state)
{
	if (state->tty_idx == active_tty_idx) {
		fb_scroll_line();
	} else {
		/* Scroll saved_text and color arrays up one row */
		unsigned row_bytes = (unsigned)MAX_COL;
		unsigned last = row_bytes * (unsigned)(MAX_ROW - 1);
		memmove(state->saved_text, state->saved_text + row_bytes, last);
		memset(state->saved_text + last, 0, row_bytes);
		memmove(state->saved_fg, state->saved_fg + row_bytes,
			last * sizeof(unsigned));
		memset(state->saved_fg + last, 0xFF,
		       row_bytes * sizeof(unsigned));
		memmove(state->saved_bg, state->saved_bg + row_bytes,
			last * sizeof(unsigned));
		memset(state->saved_bg + last, 0, row_bytes * sizeof(unsigned));
	}
}

static void tty_roll_region(tty_state *state)
{
	unsigned top = (unsigned)state->scroll_top;
	unsigned bot = (unsigned)state->scroll_bot;

	if (top == 0 && bot == state->max_row - 1) {
		tty_roll_line(state);
		return;
	}
	if (state->tty_idx == active_tty_idx) {
		fb_scroll_region(top, bot);
	} else {
		unsigned col = (unsigned)MAX_COL;
		char *t = state->saved_text + top * col;
		memmove(t, t + col, (bot - top) * col);
		memset(state->saved_text + bot * col, 0, col);
		memmove(state->saved_fg + top * col,
			state->saved_fg + (top + 1) * col,
			(bot - top) * col * sizeof(unsigned));
		memset(state->saved_fg + bot * col, 0xFF,
		       col * sizeof(unsigned));
		memmove(state->saved_bg + top * col,
			state->saved_bg + (top + 1) * col,
			(bot - top) * col * sizeof(unsigned));
		memset(state->saved_bg + bot * col, 0, col * sizeof(unsigned));
	}
}

/*
 * tty_hw_cursor - update the hardware cursor register (or framebuffer cursor).
 * Skipped for inactive TTYs — they track cursor in state->cursor only.
 */
static void tty_hw_cursor(tty_state *state, unsigned pos)
{
	if (state->cursor_hidden)
		return;
	if (state->tty_idx == active_tty_idx)
		fb_update_cursor(pos);
}

/* ── Cursor helpers ──────────────────────────────────────────────────────── */

/* Set cursor position, scrolling if past end. Does NOT update hardware. */
static void cursor_set(tty_state *state, int pos)
{
	if (pos < 0)
		return;
	state->cursor = pos;
	while (state->cursor >= MAX_CHARS) {
		tty_roll_line(state);
		state->cursor -= (int)MAX_COL;
	}
}

/* Set cursor and sync hardware register. */
static void cursor_forward(tty_state *state, int pos)
{
	cursor_set(state, pos);
	tty_hw_cursor(state, (unsigned)state->cursor);
}

/* ── Insert / delete lines ───────────────────────────────────────────────── */

static void tty_insert_lines(tty_state *state, int n)
{
	int row = CUR_ROW;
	int bot = state->scroll_bot;
	if (n < 1)
		n = 1;
	if (row > bot)
		return;
	if (n > bot - row + 1)
		n = bot - row + 1;
	if (state->tty_idx == active_tty_idx) {
		fb_insert_lines((unsigned)row, (unsigned)bot, (unsigned)n);
	} else {
		unsigned col = (unsigned)MAX_COL;
		unsigned move = (unsigned)(bot - row + 1 - n);
		if (move) {
			memmove(state->saved_text + (unsigned)(row + n) * col,
				state->saved_text + (unsigned)row * col,
				move * col);
			memmove(state->saved_fg + (unsigned)(row + n) * col,
				state->saved_fg + (unsigned)row * col,
				move * col * sizeof(unsigned));
			memmove(state->saved_bg + (unsigned)(row + n) * col,
				state->saved_bg + (unsigned)row * col,
				move * col * sizeof(unsigned));
		}
		memset(state->saved_text + (unsigned)row * col, ' ',
		       (unsigned)n * col);
		memset(state->saved_fg + (unsigned)row * col, 0xFF,
		       (unsigned)n * col * sizeof(unsigned));
		memset(state->saved_bg + (unsigned)row * col, 0,
		       (unsigned)n * col * sizeof(unsigned));
	}
}

static void tty_delete_lines(tty_state *state, int n)
{
	int row = CUR_ROW;
	int bot = state->scroll_bot;
	if (n < 1)
		n = 1;
	if (row > bot)
		return;
	if (n > bot - row + 1)
		n = bot - row + 1;
	if (state->tty_idx == active_tty_idx) {
		fb_delete_lines((unsigned)row, (unsigned)bot, (unsigned)n);
	} else {
		unsigned col = (unsigned)MAX_COL;
		unsigned move = (unsigned)(bot - row + 1 - n);
		if (move) {
			memmove(state->saved_text + (unsigned)row * col,
				state->saved_text + (unsigned)(row + n) * col,
				move * col);
			memmove(state->saved_fg + (unsigned)row * col,
				state->saved_fg + (unsigned)(row + n) * col,
				move * col * sizeof(unsigned));
			memmove(state->saved_bg + (unsigned)row * col,
				state->saved_bg + (unsigned)(row + n) * col,
				move * col * sizeof(unsigned));
		}
		memset(state->saved_text + (unsigned)(bot - n + 1) * col, ' ',
		       (unsigned)n * col);
		memset(state->saved_fg + (unsigned)(bot - n + 1) * col, 0xFF,
		       (unsigned)n * col * sizeof(unsigned));
		memset(state->saved_bg + (unsigned)(bot - n + 1) * col, 0,
		       (unsigned)n * col * sizeof(unsigned));
	}
}

static void tty_delete_chars(tty_state *state, int n)
{
	int row = CUR_ROW;
	int col = CUR_COL;
	int max_col = (int)MAX_COL;
	int i;

	if (n < 1)
		n = 1;
	if (n > max_col - col)
		n = max_col - col;
	if (state->tty_idx == active_tty_idx) {
		for (i = col; i < max_col - n; i++)
			vga_putchar(state, row, i, fb_getchar(i + n, row));
		for (i = max_col - n; i < max_col; i++)
			vga_putchar(state, row, i, ' ');
	} else {
		char *line = state->saved_text + row * max_col;
		unsigned *fg_line = state->saved_fg + row * max_col;
		unsigned *bg_line = state->saved_bg + row * max_col;
		memmove(line + col, line + col + n,
			(unsigned)(max_col - col - n));
		memset(line + max_col - n, ' ', (unsigned)n);
		memmove(fg_line + col, fg_line + col + n,
			(unsigned)(max_col - col - n) * sizeof(unsigned));
		memset(fg_line + max_col - n, 0xFF,
		       (unsigned)n * sizeof(unsigned));
		memmove(bg_line + col, bg_line + col + n,
			(unsigned)(max_col - col - n) * sizeof(unsigned));
		memset(bg_line + max_col - n, 0,
		       (unsigned)n * sizeof(unsigned));
	}
}

static void tty_insert_chars(tty_state *state, int n)
{
	int row = CUR_ROW;
	int col = CUR_COL;
	int max_col = (int)MAX_COL;
	int i;

	if (n < 1)
		n = 1;
	if (n > max_col - col)
		n = max_col - col;
	if (state->tty_idx == active_tty_idx) {
		for (i = max_col - 1; i >= col + n; i--)
			vga_putchar(state, row, i, fb_getchar(i - n, row));
		for (i = col; i < col + n; i++)
			vga_putchar(state, row, i, ' ');
	} else {
		char *line = state->saved_text + row * max_col;
		unsigned *fg_line = state->saved_fg + row * max_col;
		unsigned *bg_line = state->saved_bg + row * max_col;
		memmove(line + col + n, line + col,
			(unsigned)(max_col - col - n));
		memset(line + col, ' ', (unsigned)n);
		memmove(fg_line + col + n, fg_line + col,
			(unsigned)(max_col - col - n) * sizeof(unsigned));
		memset(fg_line + col, 0xFF, (unsigned)n * sizeof(unsigned));
		memmove(bg_line + col + n, bg_line + col,
			(unsigned)(max_col - col - n) * sizeof(unsigned));
		memset(bg_line + col, 0, (unsigned)n * sizeof(unsigned));
	}
}

/* ── Alternate screen ────────────────────────────────────────────────────── */

static void tty_enter_alt_screen(tty_state *state)
{
	unsigned sz = state->max_row * state->max_col;
	if (!state->alt_text)
		return;
	state->alt_cursor = state->cursor;
	if (state->tty_idx == active_tty_idx) {
		fb_save_text(state->alt_text, sz);
		fb_clear_screen();
	} else {
		memcpy(state->alt_text, state->saved_text, sz);
		memset(state->saved_text, 0, sz);
	}
	state->cursor = 0;
	tty_hw_cursor(state, 0);
}

static void tty_exit_alt_screen(tty_state *state)
{
	unsigned sz = state->max_row * state->max_col;
	if (!state->alt_text)
		return;
	if (state->tty_idx == active_tty_idx) {
		fb_restore_screen(state->alt_text, sz,
				  (unsigned)state->alt_cursor);
	} else {
		memcpy(state->saved_text, state->alt_text, sz);
	}
	state->cursor = state->alt_cursor;
	tty_hw_cursor(state, (unsigned)state->cursor);
}

/* ── ANSI escape state ───────────────────────────────────────────────────── */

static void ansi_begin(tty_state *state)
{
	memset(state->ansi_buf, 0, sizeof(state->ansi_buf));
	state->ansi_idx = 0;
	state->ansi_flag = 1;
}

static void ansi_end(tty_state *state)
{
	memset(state->ansi_buf, 0, sizeof(state->ansi_buf));
	state->ansi_idx = 0;
	state->ansi_flag = 0;
}

static void ansi_feed(tty_state *state, char c)
{
	char *arg = state->ansi_buf + 1;
	int val, row, col;

	switch (c) {
	case 'm': { /* SGR - select graphic rendition */
		char *p = arg;
		/* empty sequence "\e[m" means reset */
		if (*p == '\0')
			p = "0";
		while (*p) {
			char *semi = strchr(p, ';');
			if (semi)
				*semi = '\0';
			switch (atoi(p)) {
			case 0:
				state->fg_color = VGA_COLOR_WHITE;
				state->bg_color = VGA_COLOR_BLACK;
				break;
			case 30:
				state->fg_color = VGA_COLOR_BLACK;
				break;
			case 31:
				state->fg_color = VGA_COLOR_RED;
				break;
			case 32:
				state->fg_color = VGA_COLOR_GREEN;
				break;
			case 33:
				state->fg_color = VGA_COLOR_YELLOW;
				break;
			case 34:
				state->fg_color = VGA_COLOR_BLUE;
				break;
			case 35:
				state->fg_color = VGA_COLOR_MAGENTA;
				break;
			case 36:
				state->fg_color = VGA_COLOR_CYAN;
				break;
			case 37:
				state->fg_color = VGA_COLOR_WHITE;
				break;
			case 39:
				state->fg_color = VGA_COLOR_WHITE;
				break;
			/* background colors */
			case 40:
				state->bg_color = VGA_COLOR_BLACK;
				break;
			case 41:
				state->bg_color = VGA_COLOR_RED;
				break;
			case 42:
				state->bg_color = VGA_COLOR_GREEN;
				break;
			case 43:
				state->bg_color = VGA_COLOR_YELLOW;
				break;
			case 44:
				state->bg_color = VGA_COLOR_BLUE;
				break;
			case 45:
				state->bg_color = VGA_COLOR_MAGENTA;
				break;
			case 46:
				state->bg_color = VGA_COLOR_CYAN;
				break;
			case 47:
				state->bg_color = VGA_COLOR_WHITE;
				break;
			case 49:
				state->bg_color = VGA_COLOR_BLACK;
				break;
			/* bright/intense variants */
			case 90:
				state->fg_color = VGA_COLOR_GRAY;
				break;
			case 91:
				state->fg_color = VGA_COLOR_RED;
				break;
			case 92:
				state->fg_color = VGA_COLOR_GREEN;
				break;
			case 93:
				state->fg_color = VGA_COLOR_YELLOW;
				break;
			case 94:
				state->fg_color = VGA_COLOR_BLUE;
				break;
			case 95:
				state->fg_color = VGA_COLOR_MAGENTA;
				break;
			case 96:
				state->fg_color = VGA_COLOR_CYAN;
				break;
			case 97:
				state->fg_color = VGA_COLOR_WHITE;
				break;
			default:
				break;
			}
			if (!semi)
				break;
			p = semi + 1;
		}
		ansi_end(state);
		return;
	}
	case 'A': /* cursor up n */
		val = atoi(arg);
		if (val < 1)
			val = 1;
		row = CUR_ROW - val;
		if (row < 0)
			row = 0;
		cursor_set(state, row * (int)MAX_COL + CUR_COL);
		ansi_end(state);
		return;
	case 'B': /* cursor down n */
		val = atoi(arg);
		if (val < 1)
			val = 1;
		row = CUR_ROW + val;
		if (row >= (int)MAX_ROW)
			row = (int)MAX_ROW - 1;
		cursor_set(state, row * (int)MAX_COL + CUR_COL);
		ansi_end(state);
		return;
	case 'C': /* cursor right n */
		val = atoi(arg);
		if (val < 1)
			val = 1;
		col = CUR_COL + val;
		if (col >= (int)MAX_COL)
			col = (int)MAX_COL - 1;
		cursor_set(state, CUR_ROW * (int)MAX_COL + col);
		ansi_end(state);
		return;
	case 'D': /* cursor left n */
		val = atoi(arg);
		if (val < 1)
			val = 1;
		col = CUR_COL - val;
		if (col < 0)
			col = 0;
		cursor_set(state, CUR_ROW * (int)MAX_COL + col);
		ansi_end(state);
		return;
	case 'E': { /* cursor next N lines (to column 0) */
		val = atoi(arg);
		if (val < 1)
			val = 1;
		row = CUR_ROW + val;
		if (row >= (int)MAX_ROW)
			row = (int)MAX_ROW - 1;
		cursor_set(state, row * (int)MAX_COL);
		ansi_end(state);
		return;
	}
	case 'F': { /* cursor prev N lines (to column 0) */
		val = atoi(arg);
		if (val < 1)
			val = 1;
		row = CUR_ROW - val;
		if (row < 0)
			row = 0;
		cursor_set(state, row * (int)MAX_COL);
		ansi_end(state);
		return;
	}
	case 'G': { /* cursor horizontal absolute (1-based column) */
		val = atoi(arg);
		col = (val > 0 ? val - 1 : 0);
		if (col >= (int)MAX_COL)
			col = (int)MAX_COL - 1;
		cursor_set(state, CUR_ROW * (int)MAX_COL + col);
		ansi_end(state);
		return;
	}
	case 'H': /* set cursor position row;col (1-based) */
	case 'f': { /* same as H */
		char *str_col = strchr(arg, ';');
		if (!str_col) {
			row = 0;
			col = 0;
		} else {
			*str_col++ = '\0';
			row = atoi(arg) - 1;
			col = atoi(str_col) - 1;
		}
		if (row < 0)
			row = 0;
		if (row >= (int)MAX_ROW)
			row = (int)MAX_ROW - 1;
		if (col < 0)
			col = 0;
		if (col >= (int)MAX_COL)
			col = (int)MAX_COL - 1;
		cursor_set(state, row * (int)MAX_COL + col);
		ansi_end(state);
		return;
	}
	case 'J': { /* erase display: 0=cursor to end, 1=start to cursor, 2=full */
		val = atoi(arg);
		if (val == 1) {
			for (int i = 0; i <= state->cursor; i++)
				vga_putchar(state, i / (int)MAX_COL,
					    i % (int)MAX_COL, ' ');
		} else if (val == 2 || val == 3) {
			tty_do_clear(state);
		} else { /* 0 or default: cursor to end */
			for (int i = state->cursor; i < MAX_CHARS; i++)
				vga_putchar(state, i / (int)MAX_COL,
					    i % (int)MAX_COL, ' ');
		}
		ansi_end(state);
		return;
	}
	case 'K': { /* erase line: 0=to end, 1=to start, 2=whole line */
		val = atoi(arg);
		int start_col, end_col, r;
		r = CUR_ROW;
		if (val == 1) {
			start_col = 0;
			end_col = CUR_COL;
		} else if (val == 2) {
			start_col = 0;
			end_col = (int)MAX_COL - 1;
		} else { /* 0: cursor to end */
			start_col = CUR_COL;
			end_col = (int)MAX_COL - 1;
		}
		for (int c2 = start_col; c2 <= end_col; c2++)
			vga_putchar(state, r, c2, ' ');
		ansi_end(state);
		return;
	}
	case 'L': { /* IL - insert N blank lines at cursor row */
		val = atoi(arg);
		tty_insert_lines(state, val);
		ansi_end(state);
		return;
	}
	case 'M': { /* DL - delete N lines at cursor row */
		val = atoi(arg);
		tty_delete_lines(state, val);
		ansi_end(state);
		return;
	}
	case 'P': { /* DCH - delete N characters at cursor column */
		val = atoi(arg);
		tty_delete_chars(state, val);
		ansi_end(state);
		return;
	}
	case '@': { /* ICH - insert N blank characters at cursor column */
		val = atoi(arg);
		tty_insert_chars(state, val);
		ansi_end(state);
		return;
	}
	case 'c': /* ESC c = RIS (full reset); ESC [ c = DA (no-op) */
		if (state->ansi_buf[0] == '\0') {
			/* RIS: reset terminal state and clear screen */
			state->fg_color = VGA_COLOR_WHITE;
			state->bg_color = VGA_COLOR_BLACK;
			state->scroll_top = 0;
			state->scroll_bot = (int)MAX_ROW - 1;
			state->no_wrap = 0;
			state->cursor_hidden = 0;
			tty_do_clear(state);
			cursor_set(state, 0);
			tty_hw_cursor(state, 0);
		}
		ansi_end(state);
		return;
	case 'd': { /* cursor vertical absolute (1-based row) */
		val = atoi(arg);
		row = (val > 0 ? val - 1 : 0);
		if (row >= (int)MAX_ROW)
			row = (int)MAX_ROW - 1;
		cursor_set(state, row * (int)MAX_COL + CUR_COL);
		ansi_end(state);
		return;
	}
	case 'r': { /* DECSTBM - set scrolling region (1-based top;bot) */
		char *str_bot = strchr(arg, ';');
		if (!str_bot) {
			state->scroll_top = 0;
			state->scroll_bot = (int)MAX_ROW - 1;
		} else {
			*str_bot++ = '\0';
			int top = atoi(arg) - 1;
			int bot = atoi(str_bot) - 1;
			if (top < 0)
				top = 0;
			if (bot >= (int)MAX_ROW)
				bot = (int)MAX_ROW - 1;
			if (top < bot) {
				state->scroll_top = top;
				state->scroll_bot = bot;
			}
		}
		/* DECSTBM moves cursor to home */
		cursor_set(state, 0);
		ansi_end(state);
		return;
	}
	case 's': /* save cursor position */
		state->saved_cursor = state->cursor;
		ansi_end(state);
		return;
	case 'u': /* restore cursor position */
		if (state->saved_cursor >= 0)
			cursor_set(state, state->saved_cursor);
		ansi_end(state);
		return;
	case 'h': /* mode set */
	case 'l': { /* mode reset */
		int set = (c == 'h');
		if (arg[0] == '?') {
			int mode = atoi(arg + 1);
			switch (mode) {
			case 7: /* auto-wrap */
				state->no_wrap = !set;
				break;
			case 25: /* cursor visibility */
				state->cursor_hidden = !set;
				if (set)
					tty_hw_cursor(state,
						      (unsigned)state->cursor);
				break;
			case 1049: /* alternate screen buffer */
				if (set)
					tty_enter_alt_screen(state);
				else
					tty_exit_alt_screen(state);
				break;
			default:
				break;
			}
		}
		ansi_end(state);
		return;
	}
	default:
		break;
	}

	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
		ansi_end(state);
	} else if (state->ansi_idx < (int)sizeof(state->ansi_buf) - 1) {
		state->ansi_buf[state->ansi_idx++] = c;
	}
}

/* ── Character rendering ─────────────────────────────────────────────────── */

/*
 * char_to_pos - process one character and return new cursor position.
 * Returns current cursor unchanged for unrecognised characters.
 * Does NOT update cursor or hardware register.
 */
static int char_to_pos(tty_state *state, char c)
{
	int i, pos, spaces;

	if (state->ansi_flag) {
		ansi_feed(state, c);
		return state->cursor;
	}

	switch ((unsigned char)c) {
	case '\n':
		if (CUR_ROW == state->scroll_bot) {
			tty_roll_region(state);
			return state->scroll_bot * (int)MAX_COL + CUR_COL;
		}
		return (CUR_ROW + 1) * (int)MAX_COL + CUR_COL;
	case '\r':
		return CUR_ROW * (int)MAX_COL;
	case '\b':
		return state->cursor > 0 ? state->cursor - 1 : 0;
	case '\t':
		spaces = 8 - (CUR_COL % 8);
		pos = state->cursor;
		for (i = 0; i < spaces; i++) {
			vga_putchar(state, pos / (int)MAX_COL,
				    pos % (int)MAX_COL, ' ');
			pos++;
		}
		return pos;
	case 0x0c: /* form feed: clear screen */
		tty_do_clear(state);
		return state->cursor;
	case 0x1b: /* ESC: begin ANSI sequence */
		ansi_begin(state);
		return state->cursor;
	default:
		if (isprint(c)) {
			vga_putchar(state, CUR_ROW, CUR_COL, c);
			if (state->no_wrap && CUR_COL == (int)MAX_COL - 1)
				return state->cursor; /* stay at last column */
			return state->cursor + 1;
		}
		return state->cursor;
	}
}

/* ── Output with locking ─────────────────────────────────────────────────── */

/*
 * tty_raw_write - write bytes to the TTY under the spinlock.
 * Batches hardware cursor updates: one tty_hw_cursor call per write.
 */
static void tty_raw_write(tty_state *state, const char *buf, unsigned len)
{
	unsigned i;
	spinlock_lock(&state->lock);
	for (i = 0; i < len; i++)
		cursor_set(state, char_to_pos(state, buf[i]));
	tty_hw_cursor(state, (unsigned)state->cursor);
	spinlock_unlock(&state->lock);
}

static int at_col_zero(tty_state *state)
{
	return state->cursor % (int)MAX_COL == 0;
}

/* Apply c_oflag output processing and write one character. */
static void output_char(tty_state *state, unsigned char c)
{
	if (!(state->termios.c_oflag & OPOST)) {
		tty_raw_write(state, (const char *)&c, 1);
		return;
	}
	if (state->termios.c_oflag & OLCUC)
		c = (unsigned char)toupper(c);
	if (c == '\n') {
		if (state->termios.c_oflag & ONLCR) {
			tty_raw_write(state, "\r\n", 2);
			return;
		}
	} else if (c == '\r') {
		if (state->termios.c_oflag & OCRNL) {
			tty_raw_write(state, "\n", 1);
			return;
		}
		if ((state->termios.c_oflag & ONOCR) && at_col_zero(state))
			return;
	}
	tty_raw_write(state, (const char *)&c, 1);
}

/* Write a buffer with output processing. */
static void tty_do_write(tty_state *state, const char *buf, unsigned len)
{
	unsigned i;
	for (i = 0; i < len; i++)
		output_char(state, (unsigned char)buf[i]);
}

/* ── Canonical / raw input ───────────────────────────────────────────────── */

static void tty_echo_cb(void *ctx, const char *buf, int len)
{
	tty_do_write((tty_state *)ctx, buf, (unsigned)len);
}

/* Read one complete line into canon. Returns 1 on line, 0 on EOF/signal. */
static int canon_readline(tty_state *state)
{
	return tty_ldisc_canon_readline(&state->canon, &state->termios,
					state->kb_buf, 1, state->pgrp,
					tty_echo_cb, state);
}

/* Raw mode read: block until VMIN chars are available, then drain. */
static ssize_t raw_read(tty_state *state, char *dst, size_t size)
{
	const struct termios *tc = &state->termios;
	unsigned vmin = tc->c_cc[VMIN];
	ssize_t n = 0;
	while ((size_t)n < size) {
		if (state->released)
			break;
		if ((unsigned)n >= vmin && cyb_isempty(state->kb_buf))
			break;
		unsigned char raw;
		int _ret = cyb_getbuf(state->kb_buf, &raw, 1, 1, 1);
		if (_ret < 0)
			return n > 0 ? n : -EINTR;
		if (_ret == 0 || raw == (unsigned char)EOF)
			break;
		int ch = tty_input_translate(raw, tc->c_iflag);
		if (ch < 0)
			continue;
		dst[n++] = (char)ch;
		if (tc->c_lflag & ECHO)
			tty_do_write(state, &dst[n - 1], 1);
	}
	return n;
}

/* ── Public kernel interface ─────────────────────────────────────────────── */

void tty_init(void)
{
	int i, j, k;
	for (i = 0; i < TTY_MAX_VDEV; i++) {
		tty_state *t = &ttys[i];
		fb_get_char_dims(&t->max_col, &t->max_row);
		for (j = 0; j < (int)t->max_row; j++)
			for (k = 0; k < (int)t->max_col; k++)
				vga_putchar(t, j, k, ' ');
		t->cursor = 0;
		spinlock_init(&t->lock);
		t->termios = tty_default_termios;
		t->tty_idx = i;
		t->bash_pid = 0;
		t->scroll_top = 0;
		t->scroll_bot = (int)t->max_row - 1;
		t->saved_cursor = 0;
		t->fg_color = VGA_COLOR_WHITE;
		t->bg_color = VGA_COLOR_BLACK;
		t->kb_mode = K_XLATE;
	}
	spinlock_init(&tty_switch_lock);
}

/*
 * tty_default_emit_unsafe - emit one character without acquiring the lock.
 * Used as a callback by kprint; the caller (printk) holds the lock.
 */
void tty_default_emit_unsafe(char c, void *ctx)
{
	cursor_forward(this_ttys, char_to_pos(this_ttys, c));
}

void tty_lock_acquire(void)
{
	spinlock_lock(&this_ttys->lock);
}

void tty_lock_release(void)
{
	spinlock_unlock(&this_ttys->lock);
}

void tty_default_clear(void)
{
	spinlock_lock(&this_ttys->lock);
	tty_do_clear(this_ttys);
	this_ttys->cursor = 0;
	spinlock_unlock(&this_ttys->lock);
}

/* ── Active TTY keyboard input ───────────────────────────────────────────── */

void tty_active_kb_put(unsigned char c)
{
	tty_state *t;

	spinlock_lock(&tty_switch_lock);
	t = this_ttys;
	spinlock_unlock(&tty_switch_lock);

	cyb_putbuf(t->kb_buf, &c, 1, 0, 0);
}

/* ── Bash spawner helpers ────────────────────────────────────────────────── */

/*
 * tty_bash_spawner_body - kernel-thread body that execs /bin/bash on TTY n.
 *
 * Runs as a fresh kernel task created by ps_create().  Sets up the root
 * filesystem reference, opens stdin/stdout/stderr on /dev/ttyn, then
 * replaces itself with /bin/bash via sys_execve().
 */
static void tty_bash_spawner(void *p)
{
	tty_state *state = (tty_state *)p;
	char tty_path[16];
	char *argv[] = { "/bin/bash", "-l", NULL };
	char *envp[] = { "PATH=/bin:/usr/bin:/sbin", "TERM=linux", "HOME=/root",
			 "LANG=en_US", NULL };
	struct stat st;
	task_struct *cur = CURRENT_TASK();

	sprintf(tty_path, "/dev/tty%d", state->tty_idx);

	/* Wire this task to the root filesystem. */
	cur->root = state->parent->root;
	sb_get(cur->root);

	/* set parent of current so that wait can work */
	cur->parent = state->parent;

	/* Set working directory. */
	strcpy(cur->user->cwd, "/root");

	/* Set up TSS esp0 for user-mode entry. */
	ps_update_tss((unsigned)cur + PAGE_SIZE);

	/* Open stdin (0), stdout (1), stderr (2) on this TTY. */
	fs_open(tty_path, O_RDONLY, 0);
	fs_open(tty_path, O_WRONLY, 0);
	fs_open(tty_path, O_WRONLY, 0);

	/* Exec bash if it exists. */
	if (fs_stat("/bin/bash", &st) == 0)
		sys_execve("/bin/bash", argv, envp);

	/* bash not found — task will exit naturally. */
}

/* ── TTY switch ──────────────────────────────────────────────────────────── */

/*
 * tty_switch - switch the active virtual terminal to index n.
 *
 * 1. Save the current TTY's framebuffer text to its saved_text.
 * 2. Update this_ttys / active_tty_idx.
 * 3. Restore the new TTY's saved_text to the framebuffer.
 * 4. Spawn /bin/bash on TTY n if it has no live process.
 *
 * Called from the keyboard DSR (interrupts disabled) so all operations
 * must be non-blocking.
 */
void tty_switch(int n)
{
	unsigned text_size;
	tty_state *except_tty;

	if (n < 0 || n >= TTY_SWITCH_COUNT || n == active_tty_idx)
		return;

	except_tty = &ttys[n];

	spinlock_lock(&tty_switch_lock);

	text_size = this_ttys->max_row * this_ttys->max_col;

	/*
	 * Hold the outgoing TTY's output lock while saving its framebuffer so
	 * that a concurrent tty_raw_write on another CPU cannot modify the
	 * screen or cursor between our save and the pointer flip.
	 */
	spinlock_lock(&this_ttys->lock);
	fb_save_text(this_ttys->saved_text, text_size);
	fb_save_colors(this_ttys->saved_fg, this_ttys->saved_bg, text_size);
	spinlock_unlock(&this_ttys->lock);

	/* Flip active TTY — visible to other CPUs only after this point. */
	active_tty_idx = n;
	this_ttys = except_tty;

	/* Restore new TTY's cached text and colors to the framebuffer. */
	fb_restore_colors(this_ttys->saved_fg, this_ttys->saved_bg, text_size);
	fb_restore_screen(this_ttys->saved_text, text_size,
			  (unsigned)this_ttys->cursor);

	spinlock_unlock(&tty_switch_lock);

	/* Spawn bash on the new TTY if no live process is there yet. */
	if (n > 0) {
		int need_spawn = 0;

		if (except_tty->bash_pid == 0) {
			need_spawn = 1;
		} else {
			task_struct *t = ps_find_process(except_tty->bash_pid);
			if (!t || t->status == ps_dying)
				need_spawn = 1;
		}

		if (need_spawn)
			except_tty->bash_pid = ps_create(tty_bash_spawner,
							 except_tty, ps_normal,
							 ps_kernel);
	}
}

/* ── VFS file operations ─────────────────────────────────────────────────── */

static ssize_t tty_fs_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	tty_state *state = fp->f_inode->i_private;
	if (state->released)
		return -EIO;
	if (fp->f_mode != O_RDONLY && fp->f_mode != O_RDWR)
		return -EACCES;
	if (size < 1 || !buf)
		return 0;
	if (state->termios.c_lflag & ICANON) {
		if (state->canon.len == 0) {
			int r = canon_readline(state);
			if (r < 0)
				return -EINTR;
			if (r == 0)
				return 0;
		}
		return (ssize_t)tty_canon_drain(&state->canon, buf, (int)size);
	}
	return raw_read(state, buf, size);
}

static ssize_t tty_fs_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	tty_state *state = fp->f_inode->i_private;
	if (state->released)
		return -EIO;
	if (fp->f_mode != O_WRONLY && fp->f_mode != O_RDWR)
		return -EACCES;
	if (size < 1 || !buf)
		return 0;
	/* TOSTOP: if set, background processes get SIGTTOU instead of writing. */
	if ((state->termios.c_lflag & TOSTOP) && state->pgrp) {
		task_struct *cur = CURRENT_TASK();
		if (cur->user && cur->user->group_id != state->pgrp) {
			ps_send_signal_pgrp(cur->user->group_id, SIGTTOU);
			return -EINTR;
		}
	}
	tty_do_write(state, (const char *)buf, size);
	return (ssize_t)size;
}

static loff_t tty_fs_llseek(file *fp, loff_t offset, int whence)
{
	tty_state *state = fp->f_inode->i_private;
	int pos;
	switch (whence) {
	case SEEK_SET:
		pos = (int)offset;
		break;
	case SEEK_CUR:
		pos = (int)offset + state->cursor;
		break;
	case SEEK_END:
		pos = MAX_CHARS - (int)offset;
		break;
	default:
		return -EINVAL;
	}
	if (pos < 0)
		pos = 0;
	if (pos > MAX_CHARS)
		pos = MAX_CHARS;
	state->cursor = pos;
	tty_hw_cursor(state, (unsigned)state->cursor);
	fp->f_pos = state->cursor;
	return fp->f_pos;
}

static int tty_fs_poll(file *fp, unsigned type)
{
	tty_state *state = fp->f_inode->i_private;
	if (type == FS_POLL_WRITE)
		return (fp->f_mode == O_RDONLY) ? -1 : 0;
	if (type == FS_POLL_READ) {
		if (fp->f_mode == O_WRONLY)
			return -1;
		if (state->termios.c_lflag & ICANON)
			return (state->canon.len > 0) ? 0 : -1;
		return cyb_isempty(state->kb_buf) ? -1 : 0;
	}
	return -1;
}

static int tty_fs_ioctl(file *fp, unsigned cmd, void *buf)
{
	tty_state *state = fp->f_inode->i_private;
	switch (cmd) {
	case FIONREAD:
		*(int *)buf = cyb_get_buf_len(state->kb_buf);
		return 0;
	case TCGETS:
		memcpy(buf, &state->termios, sizeof(state->termios));
		return 0;
	case TCSETS:
	case TCSETSW:
		memcpy(&state->termios, buf, sizeof(state->termios));
		return 0;
	case TCSETSF:
		/* Flush pending input before applying new settings. */
		state->canon.len = 0;
		cyb_flush(state->kb_buf);
		memcpy(&state->termios, buf, sizeof(state->termios));
		return 0;
	case TIOCGWINSZ: {
		struct winsize *ws = (struct winsize *)buf;
		ws->ws_row = (unsigned short)MAX_ROW;
		ws->ws_col = (unsigned short)MAX_COL;
		ws->ws_xpixel = ws->ws_ypixel = 0;
		return 0;
	}
	case TIOCSWINSZ:
		/* window size is hardware-fixed; accept silently */
		return 0;
	case TIOCGPGRP:
		*(unsigned *)buf = state->pgrp;
		return 0;
	case TIOCSPGRP:
		state->pgrp = *(unsigned *)buf;
		return 0;
	case TIOCSCTTY: {
		/*
		 * Make this TTY the controlling terminal for the calling
		 * process's session.  The process must be a session leader.
		 * arg==1 allows stealing from another session.
		 */
		task_struct *cur = CURRENT_TASK();
		int steal = (int)buf;
		if (!cur->user || cur->user->session_id != cur->psid)
			return -EPERM; /* must be session leader */
		if (state->pgrp && !steal)
			return -EPERM; /* already owned, not stealing */
		state->pgrp = cur->user->group_id;
		return 0;
	}
	case KDGKBTYPE:
		*(unsigned char *)buf = KB_101;
		return 0;
	case KDGKBMODE:
		*(int *)buf = state->kb_mode;
		return 0;
	case KDSKBMODE:
		state->kb_mode = (int)buf;
		return 0;
	case KDGKBENT:
		return kbd_get_kbentry((struct kbentry *)buf);
	case KDSKBENT:
		return kbd_set_kbentry((const struct kbentry *)buf);
	case KDGKBSENT:
		return kbd_get_kbsentry((struct kbsentry *)buf);
	case KDSKBSENT:
		return kbd_set_kbsentry((const struct kbsentry *)buf);
	case KDGKBDIACR: {
		struct kbdiacrs *d = (struct kbdiacrs *)buf;
		d->kb_cnt = 0;
		return 0;
	}
	case KDSKBDIACR:
		return 0;
	case TIOCLINUX: {
		/*
		 * Linux virtual-console ioctl; subcommand is the first byte.
		 * Accept the call silently.
		 */
		return 0;
	}
	case KDSIGACCEPT:
		/* Process declares it will handle VT-switch signals itself.
		 * We do not implement VT switching via signals, so accept
		 * the call silently. */
		return 0;
	}
	return -ENOSYS;
}

static int tty_fs_getattr(inode *node, struct stat *s)
{
	tty_state *state = node->i_private;
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0xb;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_nlink = 1;
	s->st_rdev = (4 << 8) |
		     (unsigned)state->tty_idx; /* major 4 = virtual console */
	s->st_size = (loff_t)(MAX_CHARS - state->cursor);
	return 0;
}

static int tty_fs_release(file *fp)
{
	tty_state *state = fp->f_inode->i_private;

	if (__sync_add_and_fetch(&state->open_count, -1) == 0) {
		/*
		 * Last fd closed: mark the TTY released and wake up any task
		 * blocked in read().  We put an EOF sentinel (0xFF) into the
		 * keyboard buffer so that cyb_getbuf() returns immediately;
		 * canon_readline (check_eof=1) and raw_read both treat it as
		 * end-of-file and bail out.
		 */
		state->released = 1;
		unsigned char eof_byte = (unsigned char)EOF;
		cyb_putbuf(state->kb_buf, &eof_byte, 1, 0, 0);
	}

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const inode_operations tty_iops = {
	.getattr = tty_fs_getattr,
};

static const file_operations tty_fops = {
	.release = tty_fs_release,
	.read = tty_fs_read,
	.write = tty_fs_write,
	.llseek = tty_fs_llseek,
	.poll = tty_fs_poll,
	.ioctl = tty_fs_ioctl,
};

/* ── Filesystem registration ─────────────────────────────────────────────── */

/*
 * tty_open_state — open a file struct backed by the given tty_state.
 * Called from tty_cdev_open().
 */
static file *tty_open_state(tty_state *state, int flag)
{
	state->released = 0;
	__sync_add_and_fetch(&state->open_count, 1);

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_op = &tty_iops;
	node->i_private = state;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &tty_fops;
	return fp;
}

/*
 * tty_cdev_open — cdev dispatch callback.
 *   major 4, minor 0-9 → tty0..tty9
 *   major 5, minor 0   → /dev/tty  (controlling terminal, mapped to tty0)
 *   major 5, minor 1   → /dev/console (system console, mapped to tty0)
 */
static file *tty_cdev_open(unsigned rdev, int flag)
{
	unsigned major = MAJOR(rdev);
	unsigned minor = MINOR(rdev);
	tty_state *state;

	if (major == 4) {
		if (minor >= TTY_MAX_VDEV)
			return NULL;
		state = &ttys[minor];
	} else { /* major == 5: /dev/tty and /dev/console */
		state = &ttys[DEFAULT_TTY];
	}
	return tty_open_state(state, flag);
}

/*
 * tty_fs_init — allocate per-TTY resources.
 * Mounting is done later by tty_dev_register() via DEV_INIT.
 */
static void tty_fs_init(void)
{
	for (int i = 0; i < TTY_MAX_VDEV; i++) {
		tty_state *t = &ttys[i];
		t->kb_buf = cyb_create(1);
		t->saved_text = (char *)zalloc(t->max_row * t->max_col);
		unsigned color_sz = t->max_row * t->max_col * sizeof(unsigned);
		t->saved_fg = (unsigned *)zalloc(color_sz);
		t->saved_bg = (unsigned *)zalloc(color_sz);
		memset(t->saved_fg, 0xFF, color_sz);
		t->alt_text = (char *)zalloc(t->max_row * t->max_col);
		if (i > 0)
			t->parent = ps_find_process(1);
	}
}

static void tty_dev_register(super_block *dev_sb)
{
	char path[16];
	int i;

	/* major 4: tty0..tty9 */
	cdev_register(S_IFCHR, 4, 0, TTY_MAX_VDEV, tty_cdev_open);
	for (i = 0; i < TTY_MAX_VDEV; i++) {
		sprintf(path, "/tty%d", i);
		vfs_mknod(dev_sb, path, S_IFCHR | 0620, MKDEV(4, i));
	}

	/* major 5: /dev/tty (minor 0) and /dev/console (minor 1) */
	cdev_register(S_IFCHR, 5, 0, 2, tty_cdev_open);
	vfs_mknod(dev_sb, "/tty", S_IFCHR | 0620, MKDEV(5, 0));
	vfs_mknod(dev_sb, "/console", S_IFCHR | 0600, MKDEV(5, 1));
}

KERNEL_INIT(4, tty_fs_init);
DEV_INIT(tty_dev_register);
