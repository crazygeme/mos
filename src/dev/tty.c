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
#include <int/dsr.h>
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
#include "devnums.h"
#include "pts_internal.h"
#include "tty_ldisc.h"
#include <hw/keyboard.h>

#define TTY_SWITCH_COUNT 10 /* how many TTYs support switching (1-10) */

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
	char ansi_buf[24];
	int ansi_idx;
	/* canonical input line buffer */
	tty_canon_t canon;
	int canon_ready;
	/* per-TTY keyboard input queue */
	cy_buf *kb_buf;
	/* unified cell buffer (character + fg/bg per cell) */
	tty_cell_t *cells;
	/* TTY index (1..TTY_MAX_VDEV) */
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
	tty_cell_t *alt_cells;
	int alt_cursor;
	int alt_active;
	/* saved raw graphics framebuffer while another VT is visible */
	void *graphics_fb;
	unsigned graphics_fb_size;
	/* current foreground/background colors (set by SGR escape sequences) */
	unsigned fg_color;
	unsigned bg_color;
	/* keyboard translation mode (K_XLATE, K_RAW, etc.) */
	int kb_mode;
	/* keyboard LED bitmap from KDSETLED/KDGETLED */
	int kb_leds;
	/* keyboard repeat settings returned by KDKBDREP */
	struct kbd_repeat kb_repeat;
	/* console display mode selected through KDSETMODE */
	int kd_mode;
	/* process that most recently switched this VT into KD_GRAPHICS */
	unsigned kd_owner_pid;
	/* virtual-terminal mode configured via VT_SETMODE */
	struct vt_mode vt_mode;
} tty_state;

#define DEFAULT_TTY 1
static tty_state ttys[TTY_MAX_VDEV];
static int active_tty_idx = DEFAULT_TTY;
static tty_state *this_ttys = NULL;
static unsigned _displayed_cursor; /* linear position of cursor drawn on screen */
/* Deferred VT target while a VT_PROCESS owner is deciding whether to release. */

/*
 * tty_switch_lock - global spinlock protecting this_ttys, active_tty_idx,
 * and the framebuffer save/restore during a TTY switch.  Also acquired by
 * tty_active_kb_put() so keyboard input is always routed to the correct TTY
 * even when a switch is in progress on another CPU.
 */
static spinlock_t tty_switch_lock;
static volatile int tty_graphics_refresh_pending;

static int tty_fb_text_is_visible(const tty_state *state);
static void tty_exit_alt_screen(tty_state *state);
static void tty_bash_spawner(void *p);
static void tty_sync_fb_mode_all(void);
static void tty_switch_internal(int n, int spawn_shell);
static void tty_complete_switch_locked(int n, int spawn_shell);
static void tty_capture_graphics_locked(tty_state *state);
static void tty_restore_graphics_locked(tty_state *state);
static void tty_graphics_refresh_dsr(void *unused);

#define VGA_IO_FIRST 0x3b4
#define VGA_IO_LAST 0x3df
#define VGA_IO_COUNT (VGA_IO_LAST - VGA_IO_FIRST + 1)

static int tty_file_nonblock(file *fp)
{
	return (fp->f_flag & O_NONBLOCK) != 0;
}

/*
 * tty_restore_text_console_locked - bring one VT back to normal text-console
 * semantics after a graphics user (typically X) is done with it.
 *
 * This function intentionally restores only VT/TTY state. It does not redraw
 * the screen and it does not switch VTs by itself. Callers decide whether the
 * VT is currently visible and, if it is, whether the framebuffer mode must be
 * resynchronized before issuing fb_redraw().
 *
 * Important details:
 * - KD_TEXT means text rendering is again allowed on this VT.
 * - kd_owner_pid is cleared so later close/exit paths do not think an old
 *   graphics owner is still responsible for cleanup.
 * - kb_mode is reset to K_XLATE because X often leaves the console in a raw
 *   keyboard mode that would make the shell unusable.
 * - The alternate screen is exited so shell text uses the normal saved buffer.
 */
static void tty_restore_text_console_locked(tty_state *state)
{
	state->kd_mode = KD_TEXT;
	state->kd_owner_pid = 0;
	state->kb_mode = K_XLATE;
	state->cursor_hidden = 0;
	state->vt_mode.mode = VT_AUTO;
	if (state->alt_active)
		tty_exit_alt_screen(state);
}

/*
 * tty_complete_switch_locked - commit a VT switch once the kernel has
 * permission to do so.
 *
 * There are two ways to reach here:
 * 1. Immediate switch: the current VT is VT_AUTO, so the kernel can move to
 *    the target right away.
 * This helper does every side effect that should happen exactly once for a
 * completed switch:
 * - update active_tty_idx / this_ttys
 * - reconcile tty geometry with the live framebuffer mode if the destination
 *   VT is a text VT
 * - redraw the destination text console if text is visible there
 * - lazily spawn the default shell on unopened text VTs when requested
 *
 * Keeping this in one helper is important because every switch path must
 * perform the exact same state transition.
 */
static void tty_complete_switch_locked(int n, int spawn_shell)
{
	tty_state *except_tty = &ttys[n - 1];

	/* Finalize the visible VT change after either an immediate switch or a
	 * later VT_RELDISP(1) from the old VT's userspace owner. */
	active_tty_idx = n;
	this_ttys = except_tty;
	/*
	 * A text VT must follow the current hardware mode, not the boot-time
	 * VGA_RESOLUTION_* constants. If X changed the real resolution while it
	 * owned another VT, sync all tty buffers before redrawing this one.
	 */
	if (tty_fb_text_is_visible(this_ttys))
		tty_sync_fb_mode_all();

	_displayed_cursor = (unsigned)this_ttys->cursor;
	/* Graphics VTs intentionally skip fb_redraw(); only text VTs restore the
	 * saved console buffer through the framebuffer text renderer. */
	if (tty_fb_text_is_visible(this_ttys))
		fb_redraw(this_ttys->cells, this_ttys->max_col,
			  this_ttys->max_row, (unsigned)this_ttys->cursor);
	else if (this_ttys->kd_mode == KD_GRAPHICS)
		tty_restore_graphics_locked(this_ttys);

	if (spawn_shell && n > 1) {
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

static int tty_fb_text_is_visible(const tty_state *state)
{
	return state->tty_idx == active_tty_idx && state->kd_mode == KD_TEXT;
}

static void tty_capture_graphics_locked(tty_state *state)
{
	unsigned need;
	void *newbuf;

	if (!state || state->kd_mode != KD_GRAPHICS)
		return;

	fb_sync_mode();
	need = fb_snapshot_size();
	if (need == 0)
		return;
	if (!state->graphics_fb || state->graphics_fb_size < need) {
		newbuf = kmalloc(need);
		if (!newbuf)
			return;
		kfree(state->graphics_fb);
		state->graphics_fb = newbuf;
		state->graphics_fb_size = need;
	}
	fb_snapshot_save(state->graphics_fb, state->graphics_fb_size);
}

static void tty_restore_graphics_locked(tty_state *state)
{
	if (!state || state->kd_mode != KD_GRAPHICS || !state->graphics_fb)
		return;

	fb_sync_mode();
	if (fb_snapshot_size() > state->graphics_fb_size)
		return;
	fb_snapshot_restore(state->graphics_fb, state->graphics_fb_size);
}

/*
 * Run the expensive "present graphics VT" step outside hard interrupt
 * context. The VESA X driver writes straight into the mmap'd linear
 * framebuffer and expects scanout to reflect those writes automatically.
 * VMware SVGA instead needs an explicit UPDATE command, so we queue a DSR
 * that flushes the visible graphics VT from scheduler/task context.
 */
static void tty_graphics_refresh_dsr(void *unused)
{
	int irq;

	(void)unused;
	spinlock_lock(&tty_switch_lock, &irq);
	tty_graphics_refresh_pending = 0;
	if (this_ttys && this_ttys->kd_mode == KD_GRAPHICS)
		fb_flush();
	spinlock_unlock(&tty_switch_lock, irq);
}

static int tty_vt_is_available(int tty_idx)
{
	tty_state *state;
	task_struct *task;

	if (tty_idx < 1 || tty_idx > TTY_MAX_VDEV)
		return 0;
	if (tty_idx == active_tty_idx)
		return 0;

	state = &ttys[tty_idx - 1];
	if (state->open_count > 0)
		return 0;
	if (state->bash_pid == 0)
		return 1;

	task = ps_find_process(state->bash_pid);
	return !task || task->status == ps_dying;
}

static int tty_vt_find_free(void)
{
	int tty_idx;

	for (tty_idx = 2; tty_idx <= TTY_MAX_VDEV; tty_idx++) {
		if (tty_vt_is_available(tty_idx))
			return tty_idx;
	}

	return -1;
}

static tty_state *tty_find_controlling(task_struct *task)
{
	int tty_idx;

	if (!task || !task->user)
		return NULL;

	for (tty_idx = 1; tty_idx <= TTY_MAX_VDEV; tty_idx++) {
		tty_state *state = &ttys[tty_idx - 1];

		if (state->pgrp == task->user->group_id)
			return state;
	}

	return NULL;
}

static unsigned short tty_vt_state_mask(void)
{
	unsigned short mask = 0;
	int tty_idx;

	for (tty_idx = 1; tty_idx <= TTY_MAX_VDEV; tty_idx++) {
		tty_state *state = &ttys[tty_idx - 1];
		task_struct *task = state->bash_pid ?
					    ps_find_process(state->bash_pid) :
					    NULL;
		int allocated = (tty_idx == active_tty_idx) ||
				(state->open_count > 0) ||
				(state->bash_pid != 0 && task &&
				 task->status != ps_dying);

		if (allocated)
			mask |= (unsigned short)(1U << (tty_idx - 1));
	}

	return mask;
}

/* ── Convenience macros ──────────────────────────────────────────────────── */

#define MAX_ROW (state->max_row)
#define MAX_COL (state->max_col)
#define MAX_CHARS ((int)(MAX_ROW * MAX_COL))
#define CUR_ROW (state->cursor / (int)MAX_COL)
#define CUR_COL (state->cursor % (int)MAX_COL)

/* ── VGA/framebuffer primitives ──────────────────────────────────────────── */
static const tty_cell_t blank = { ' ', VGA_COLOR_WHITE, VGA_COLOR_BLACK };

static void vga_putchar(tty_state *state, int row, int col, char c)
{
	int idx = row * (int)MAX_COL + col;

	if (col < 0 || col >= (int)MAX_COL || row < 0 || row >= (int)MAX_ROW)
		return;
	state->cells[idx].ch = c;
	state->cells[idx].fg = state->fg_color;
	state->cells[idx].bg = state->bg_color;
	if (tty_fb_text_is_visible(state))
		fb_putcell(&state->cells[idx], col, row);
}

static void tty_do_clear(tty_state *state)
{
	unsigned sz = (unsigned)(MAX_ROW * (int)MAX_COL);
	unsigned i;

	for (i = 0; i < sz; i++)
		memcpy(&state->cells[i], &blank, sizeof(tty_cell_t));

	if (tty_fb_text_is_visible(state))
		fb_clear_screen();
}

static void tty_roll_line(tty_state *state)
{
	unsigned col = (unsigned)MAX_COL;
	unsigned total = (unsigned)(MAX_ROW * (int)MAX_COL);
	unsigned i;

	if (tty_fb_text_is_visible(state))
		fb_cursor_erase(_displayed_cursor, state->cells, col);

	memmove(state->cells, state->cells + col,
		(total - col) * sizeof(tty_cell_t));
	for (i = total - col; i < total; i++)
		memcpy(&state->cells[i], &blank, sizeof(tty_cell_t));

	if (tty_fb_text_is_visible(state))
		fb_scroll_line_px();
}

static void tty_roll_region(tty_state *state)
{
	unsigned top = (unsigned)state->scroll_top;
	unsigned bot = (unsigned)state->scroll_bot;
	unsigned col = (unsigned)MAX_COL;
	unsigned i;

	if (top == 0 && bot == (unsigned)MAX_ROW - 1) {
		tty_roll_line(state);
		return;
	}

	if (tty_fb_text_is_visible(state))
		fb_cursor_erase(_displayed_cursor, state->cells, col);

	memmove(state->cells + top * col, state->cells + (top + 1) * col,
		(bot - top) * col * sizeof(tty_cell_t));
	for (i = bot * col; i < (bot + 1) * col; i++)
		memcpy(&state->cells[i], &blank, sizeof(tty_cell_t));

	if (tty_fb_text_is_visible(state))
		fb_scroll_region_px(top, bot);
}

static void tty_hw_cursor(tty_state *state, unsigned pos)
{
	if (state->cursor_hidden || !tty_fb_text_is_visible(state))
		return;
	fb_cursor_update(_displayed_cursor, pos, state->cells,
			 (unsigned)MAX_COL);
	_displayed_cursor = pos;
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
	unsigned col = (unsigned)MAX_COL;
	unsigned move, i;

	if (n < 1)
		n = 1;
	if (row > bot)
		return;
	if (n > bot - row + 1)
		n = bot - row + 1;

	move = (unsigned)(bot - row + 1 - n);
	if (move) {
		memmove(state->cells + (unsigned)(row + n) * col,
			state->cells + (unsigned)row * col,
			move * col * sizeof(tty_cell_t));
	}
	for (i = (unsigned)row * col; i < (unsigned)(row + n) * col; i++) {
		state->cells[i].ch = ' ';
		state->cells[i].fg = VGA_COLOR_WHITE;
		state->cells[i].bg = VGA_COLOR_BLACK;
	}
	if (tty_fb_text_is_visible(state)) {
		fb_cursor_erase(_displayed_cursor, state->cells,
				(unsigned)MAX_COL);
		fb_insert_lines_px((unsigned)row, (unsigned)bot, (unsigned)n);
	}
}

static void tty_delete_lines(tty_state *state, int n)
{
	int row = CUR_ROW;
	int bot = state->scroll_bot;
	unsigned col = (unsigned)MAX_COL;
	unsigned move, clear_start, i;

	if (n < 1)
		n = 1;
	if (row > bot)
		return;
	if (n > bot - row + 1)
		n = bot - row + 1;

	move = (unsigned)(bot - row + 1 - n);
	if (move) {
		memmove(state->cells + (unsigned)row * col,
			state->cells + (unsigned)(row + n) * col,
			move * col * sizeof(tty_cell_t));
	}
	clear_start = (unsigned)(bot - n + 1) * col;
	for (i = clear_start; i < clear_start + (unsigned)n * col; i++) {
		state->cells[i].ch = ' ';
		state->cells[i].fg = VGA_COLOR_WHITE;
		state->cells[i].bg = VGA_COLOR_BLACK;
	}
	if (tty_fb_text_is_visible(state)) {
		fb_cursor_erase(_displayed_cursor, state->cells,
				(unsigned)MAX_COL);
		fb_delete_lines_px((unsigned)row, (unsigned)bot, (unsigned)n);
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

	memmove(state->cells + row * max_col + col,
		state->cells + row * max_col + col + n,
		(unsigned)(max_col - col - n) * sizeof(tty_cell_t));
	for (i = max_col - n; i < max_col; i++) {
		state->cells[row * max_col + i].ch = ' ';
		state->cells[row * max_col + i].fg = VGA_COLOR_WHITE;
		state->cells[row * max_col + i].bg = VGA_COLOR_BLACK;
	}
	if (tty_fb_text_is_visible(state)) {
		for (i = col; i < max_col; i++) {
			tty_cell_t *c = &state->cells[row * max_col + i];
			fb_putcell(c, i, row);
		}
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

	memmove(state->cells + row * max_col + col + n,
		state->cells + row * max_col + col,
		(unsigned)(max_col - col - n) * sizeof(tty_cell_t));
	for (i = col; i < col + n; i++) {
		state->cells[row * max_col + i].ch = ' ';
		state->cells[row * max_col + i].fg = VGA_COLOR_WHITE;
		state->cells[row * max_col + i].bg = VGA_COLOR_BLACK;
	}
	if (tty_fb_text_is_visible(state)) {
		for (i = col; i < max_col; i++) {
			tty_cell_t *c = &state->cells[row * max_col + i];
			fb_putcell(c, i, row);
		}
	}
}

/* ── Alternate screen ────────────────────────────────────────────────────── */

static void tty_enter_alt_screen(tty_state *state)
{
	unsigned sz = state->max_row * state->max_col;
	unsigned i;

	if (!state->alt_cells)
		return;
	state->alt_cursor = state->cursor;
	memcpy(state->alt_cells, state->cells, sz * sizeof(tty_cell_t));
	for (i = 0; i < sz; i++) {
		state->cells[i].ch = ' ';
		state->cells[i].fg = VGA_COLOR_WHITE;
		state->cells[i].bg = VGA_COLOR_BLACK;
	}
	state->cursor = 0;
	state->alt_active = 1;
	if (tty_fb_text_is_visible(state))
		fb_clear_screen();
	tty_hw_cursor(state, 0);
}

static void tty_exit_alt_screen(tty_state *state)
{
	unsigned sz = state->max_row * state->max_col;

	if (!state->alt_cells)
		return;
	memcpy(state->cells, state->alt_cells, sz * sizeof(tty_cell_t));
	state->cursor = state->alt_cursor;
	state->alt_active = 0;
	if (tty_fb_text_is_visible(state))
		fb_redraw(state->cells, state->max_col, state->max_row,
			  (unsigned)state->cursor);
	tty_hw_cursor(state, (unsigned)state->cursor);
}

/* ── ANSI escape state ───────────────────────────────────────────────────── */

/* Map xterm 256-color index to ARGB. */
static unsigned color256(int n)
{
	static const unsigned basic16[16] = {
		VGA_COLOR_BLACK,
		VGA_COLOR_RED,
		VGA_COLOR_GREEN,
		VGA_COLOR_YELLOW,
		VGA_COLOR_BLUE,
		VGA_COLOR_MAGENTA,
		VGA_COLOR_CYAN,
		VGA_COLOR_WHITE,
		ARGB(0xff, 0x55, 0x55, 0x55), /* bright black (dark gray) */
		ARGB(0xff, 0xff, 0x55, 0x55), /* bright red   */
		ARGB(0xff, 0x55, 0xff, 0x55), /* bright green */
		ARGB(0xff, 0xff, 0xff, 0x55), /* bright yellow */
		ARGB(0xff, 0x55, 0x55, 0xff), /* bright blue  */
		ARGB(0xff, 0xff, 0x55, 0xff), /* bright magenta */
		ARGB(0xff, 0x55, 0xff, 0xff), /* bright cyan  */
		VGA_COLOR_WHITE,
	};
	static const unsigned char cube[6] = { 0, 95, 135, 175, 215, 255 };

	if (n < 0)
		n = 0;
	if (n > 255)
		n = 255;
	if (n < 16)
		return basic16[n];
	if (n < 232) {
		int i = n - 16;
		int b = i % 6;
		i /= 6;
		int g = i % 6;
		i /= 6;
		int r = i;
		return ARGB(0xff, cube[r], cube[g], cube[b]);
	}
	/* Grayscale ramp 232-255: 8, 18, 28, … 238 */
	int gray = 8 + (n - 232) * 10;
	return ARGB(0xff, gray, gray, gray);
}

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
		/* Parse all semicolon-separated params into an array. */
		int params[8];
		int nparams = 0;
		char *p = arg;
		if (*p == '\0') {
			params[0] = 0;
			nparams = 1;
		} else {
			while (*p && nparams < 8) {
				char *semi = strchr(p, ';');
				if (semi)
					*semi = '\0';
				params[nparams++] = atoi(p);
				if (!semi)
					break;
				p = semi + 1;
			}
		}
		int pi = 0;
		while (pi < nparams) {
			int v = params[pi];
			/* 256-color: 38;5;N (fg) or 48;5;N (bg) */
			if ((v == 38 || v == 48) && pi + 2 < nparams &&
			    params[pi + 1] == 5) {
				unsigned col = color256(params[pi + 2]);
				if (v == 38)
					state->fg_color = col;
				else
					state->bg_color = col;
				pi += 3;
				continue;
			}
			switch (v) {
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
			pi++;
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
	case 'M': {
		if (state->ansi_buf[0] == '\0') {
			/* ESC M = RI: reverse index — cursor up, scroll if at top */
			if (CUR_ROW == state->scroll_top)
				tty_insert_lines(state, 1);
			else if (CUR_ROW > 0)
				cursor_set(state, (CUR_ROW - 1) * (int)MAX_COL +
							  CUR_COL);
		} else {
			/* CSI M = DL: delete N lines at cursor row */
			val = atoi(arg);
			tty_delete_lines(state, val);
		}
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
 * process_one_char - process one character and return new cursor position.
 * Returns current cursor unchanged for unrecognised characters.
 * Does NOT update cursor or hardware register.
 */
static int process_one_char(tty_state *state, char c)
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
 * tty_raw_write - write bytes to the TTY (caller must hold state->lock).
 * Does NOT update the hardware cursor; the caller is responsible for
 * calling tty_hw_cursor once after all characters have been processed.
 */
static void tty_raw_write(tty_state *state, const char *buf, unsigned len)
{
	unsigned i;
	int cur_pos;
	for (i = 0; i < len; i++) {
		cur_pos = process_one_char(state, buf[i]);
		cursor_set(state, cur_pos);
	}
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
	int irq;
	spinlock_lock(&state->lock, &irq);
	for (i = 0; i < len; i++)
		output_char(state, (unsigned char)buf[i]);
	tty_hw_cursor(state, (unsigned)state->cursor);
	spinlock_unlock(&state->lock, irq);
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
					state->kb_buf, 1, 1, state->pgrp,
					tty_echo_cb, state);
}

/* Raw mode read: block until VMIN chars are available, then drain. */
static ssize_t raw_read(tty_state *state, char *dst, size_t size, int blocking)
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
		int _ret = cyb_getbuf(state->kb_buf, &raw, 1, blocking, 1);
		if (_ret < 0)
			return n > 0 ? n : -EINTR;
		if (_ret == 0) {
			if (!blocking && n == 0)
				return -EAGAIN;
			break;
		}
		if (raw == (unsigned char)EOF)
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
	int i;
	for (i = 0; i < TTY_MAX_VDEV; i++) {
		tty_state *t = &ttys[i];
		fb_get_char_dims(&t->max_col, &t->max_row);
		t->cursor = 0;
		spinlock_init(&t->lock);
		t->termios = tty_default_termios;
		t->canon_ready = 0;
		t->tty_idx = i + 1;
		t->bash_pid = 0;
		t->scroll_top = 0;
		t->scroll_bot = (int)t->max_row - 1;
		t->saved_cursor = 0;
		t->alt_active = 0;
		t->graphics_fb = NULL;
		t->graphics_fb_size = 0;
		t->fg_color = VGA_COLOR_WHITE;
		t->bg_color = VGA_COLOR_BLACK;
		t->kb_mode = K_XLATE;
		t->kb_leds = 0;
		t->kb_repeat.delay = 250;
		t->kb_repeat.period = 33;
		t->kd_mode = KD_TEXT;
		t->kd_owner_pid = 0;
		t->vt_mode.mode = VT_AUTO;
		t->vt_mode.waitv = 0;
		t->vt_mode.relsig = 0;
		t->vt_mode.acqsig = 0;
		t->vt_mode.frsig = 0;
	}
	spinlock_init(&tty_switch_lock);
}

/*
 * tty_default_emit_unsafe - emit one character without acquiring the lock.
 * Used as a callback by kprint; the caller (printk) holds the lock.
 */
void tty_default_emit_unsafe(char c, void *ctx)
{
	int cur_pos;

	if (!this_ttys)
		return;

	cur_pos = process_one_char(this_ttys, c);
	cursor_forward(this_ttys, cur_pos);
}

void tty_lock_acquire(int *irq)
{
	if (!this_ttys)
		return;

	spinlock_lock(&this_ttys->lock, irq);
}

void tty_lock_release(int irq)
{
	if (!this_ttys)
		return;

	spinlock_unlock(&this_ttys->lock, irq);
}

void tty_default_clear(void)
{
	int irq;

	if (!this_ttys)
		return;

	spinlock_lock(&this_ttys->lock, &irq);
	tty_do_clear(this_ttys);
	this_ttys->cursor = 0;
	spinlock_unlock(&this_ttys->lock, irq);
}

char *tty_test_snapshot(unsigned *len_out)
{
	tty_state *state;
	tty_cell_t *cells;
	char *buf;
	char *p;
	unsigned rows, cols, cell_count;
	unsigned size;
	int irq;
	int cursor, saved_cursor, scroll_top, scroll_bot;
	int cursor_hidden, no_wrap, alt_active;
	unsigned fg, bg;
	unsigned r, c;

	if (len_out)
		*len_out = 0;

	state = this_ttys;
	if (!state)
		return NULL;

	rows = state->max_row;
	cols = state->max_col;
	cell_count = rows * cols;
	cells = kmalloc(cell_count * sizeof(*cells));
	if (!cells)
		return NULL;

	spinlock_lock(&state->lock, &irq);
	memcpy(cells, state->cells, cell_count * sizeof(*cells));
	cursor = state->cursor;
	saved_cursor = state->saved_cursor;
	scroll_top = state->scroll_top;
	scroll_bot = state->scroll_bot;
	cursor_hidden = state->cursor_hidden;
	no_wrap = state->no_wrap;
	alt_active = state->alt_active;
	fg = state->fg_color;
	bg = state->bg_color;
	spinlock_unlock(&state->lock, irq);

	size = 256 + cell_count * 48;
	buf = kmalloc(size);
	if (!buf) {
		kfree(cells);
		return NULL;
	}

	p = buf;
	p += sprintf(p,
		     "meta rows=%u cols=%u cursor_row=%u cursor_col=%u "
		     "saved_row=%u saved_col=%u scroll_top=%d scroll_bot=%d "
		     "cursor_hidden=%d no_wrap=%d alt_active=%d fg=%x bg=%x\n",
		     rows, cols, (unsigned)(cursor / (int)cols),
		     (unsigned)(cursor % (int)cols),
		     (unsigned)(saved_cursor / (int)cols),
		     (unsigned)(saved_cursor % (int)cols), scroll_top,
		     scroll_bot, cursor_hidden, no_wrap, alt_active, fg, bg);
	for (r = 0; r < rows; r++) {
		for (c = 0; c < cols; c++) {
			tty_cell_t *cell = &cells[r * cols + c];
			p += sprintf(p, "cell %u %u ch=%02x fg=%x bg=%x\n", r,
				     c, (unsigned char)cell->ch, cell->fg,
				     cell->bg);
		}
	}

	if (len_out)
		*len_out = (unsigned)(p - buf);
	kfree(cells);
	return buf;
}

/* ── Active TTY keyboard input ───────────────────────────────────────────── */

void tty_active_kb_put(unsigned char c)
{
	tty_state *t;
	int irq;

	if (!this_ttys)
		return;

	spinlock_lock(&tty_switch_lock, &irq);
	t = this_ttys;
	spinlock_unlock(&tty_switch_lock, irq);

	/* Process signal characters only for translated console input.
	 * Raw/mediumraw keyboard modes are consumed directly by applications
	 * such as X, so bytes must pass through unchanged. */
	if ((t->kb_mode == K_XLATE || t->kb_mode == K_UNICODE) &&
	    (t->termios.c_lflag & ISIG) && t->pgrp) {
		int sig = 0;
		if (t->termios.c_cc[VINTR] && c == t->termios.c_cc[VINTR])
			sig = SIGINT;
		else if (t->termios.c_cc[VQUIT] && c == t->termios.c_cc[VQUIT])
			sig = SIGQUIT;
		else if (t->termios.c_cc[VSUSP] && c == t->termios.c_cc[VSUSP])
			sig = SIGTSTP;
		if (sig) {
			ps_send_signal_pgrp(t->pgrp, sig);
			return;
		}
	}

	cyb_putbuf(t->kb_buf, &c, 1, 0, 0);
}

int tty_active_kb_mode(void)
{
	int irq;
	int mode = K_XLATE;

	if (!this_ttys)
		return mode;

	spinlock_lock(&tty_switch_lock, &irq);
	if (this_ttys)
		mode = this_ttys->kb_mode;
	spinlock_unlock(&tty_switch_lock, irq);

	return mode;
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

	/* Wire this task to the root filesystem. */
	cur->root = state->parent->root;
	sb_get(cur->root);

	/* set parent of current so that wait can work */
	cur->ppid = state->parent->psid;
	cur->user->gid = state->parent->user->gid;
	cur->user->uid = state->parent->user->uid;
	cur->user->euid = state->parent->user->euid;
	cur->user->suid = state->parent->user->suid;
	cur->user->egid = state->parent->user->egid;
	cur->user->sgid = state->parent->user->sgid;
	cur->user->fsuid = state->parent->user->fsuid;
	cur->user->fsgid = state->parent->user->fsgid;
	cur->user->session_id = 0;
	cur->user->group_id = 0;

	if (!TestControl.bash)
		/* On System V init system, VT is enabled and tty is dynamically allocated */
		sprintf(tty_path, "/dev/tty%d", tty_vt_find_free());

	else
		/* But in bare metal bash mode, we just simplly use ttyN for sessionN without login */
		sprintf(tty_path, "/dev/tty%d", state->tty_idx);

	/* Set working directory. */
	strcpy(cur->user->cwd, "/root");
	strcpy(cur->user->root_path, "/");

	/* Set up TSS esp0 for user-mode entry. */
	ps_update_tss((unsigned)cur + PAGE_SIZE);

	/* Open stdin (0), stdout (1), stderr (2) on this TTY. */
	fs_open(tty_path, O_RDONLY, 0);
	fs_open(tty_path, O_WRONLY, 0);
	fs_open(tty_path, O_RDWR, 0);

	/* Exec bash if it exists. */
	if (fs_stat("/bin/bash", &st) == 0)
		sys_execve("/bin/bash", argv, envp);

	/* bash not found — task will exit naturally. */
}

/*
 * tty_resize_one - resize one VT's saved text buffers to a new character grid.
 *
 * The framebuffer drivers expose dimensions in character cells, not pixels.
 * Whenever the real display mode changes, every VT's saved text state must be
 * reshaped so switching later does not redraw with stale geometry.
 *
 * We preserve the overlapping top-left region of both the normal and alternate
 * screen buffers and blank-fill any newly created space. This mirrors the
 * practical behavior expected by shells: old text survives where it still fits,
 * while newly visible areas start empty.
 *
 * Cursor and scroll-region metadata are then clamped into the new bounds. That
 * prevents later cursor updates or scrolling from indexing outside the resized
 * buffers after a mode change.
 */
static void tty_resize_one(tty_state *state, unsigned new_col, unsigned new_row)
{
	tty_cell_t *new_cells;
	tty_cell_t *new_alt_cells;
	unsigned old_col = state->max_col;
	unsigned old_row = state->max_row;
	unsigned copy_col;
	unsigned copy_row;
	unsigned r;

	if (!state->cells || !state->alt_cells)
		return;
	if (old_col == new_col && old_row == new_row)
		return;

	new_cells = zalloc(new_col * new_row * sizeof(*new_cells));
	new_alt_cells = zalloc(new_col * new_row * sizeof(*new_alt_cells));
	if (!new_cells || !new_alt_cells) {
		kfree(new_cells);
		kfree(new_alt_cells);
		return;
	}

	for (r = 0; r < new_row * new_col; r++) {
		new_cells[r] = blank;
		new_alt_cells[r] = blank;
	}

	copy_col = old_col < new_col ? old_col : new_col;
	copy_row = old_row < new_row ? old_row : new_row;
	for (r = 0; r < copy_row; r++) {
		memcpy(new_cells + r * new_col, state->cells + r * old_col,
		       copy_col * sizeof(*new_cells));
		memcpy(new_alt_cells + r * new_col,
		       state->alt_cells + r * old_col,
		       copy_col * sizeof(*new_alt_cells));
	}

	kfree(state->cells);
	kfree(state->alt_cells);
	state->cells = new_cells;
	state->alt_cells = new_alt_cells;
	state->max_col = new_col;
	state->max_row = new_row;
	if (state->cursor >= (int)(new_col * new_row))
		state->cursor = (int)(new_col * new_row) - 1;
	if (state->cursor < 0)
		state->cursor = 0;
	if (state->saved_cursor >= (int)(new_col * new_row))
		state->saved_cursor = state->cursor;
	state->scroll_top = 0;
	state->scroll_bot = (int)new_row - 1;
}

/*
 * tty_sync_fb_mode_all - synchronize tty geometry with the framebuffer's live
 * hardware mode.
 *
 * This is the bridge between the graphics drivers and the VT layer:
 * - fb_sync_mode() asks the active framebuffer driver to reread the current
 *   hardware mode. For example, after startx, the device may now be running at
 *   a larger resolution than the kernel's init-time mode.
 * - fb_get_char_dims() converts that pixel mode into a text grid using the
 *   current font metrics.
 * - every VT is resized to that grid so future VT switches remain safe and all
 *   saved text buffers agree on the same geometry.
 *
 * We intentionally resize every VT, not only the visible one. Otherwise a
 * later switch to an inactive VT could redraw with stale rows/cols and either
 * truncate text or walk past buffer boundaries.
 */
static void tty_sync_fb_mode_all(void)
{
	unsigned new_col, new_row;
	int i;

	fb_sync_mode();
	fb_get_char_dims(&new_col, &new_row);
	if (new_col == 0 || new_row == 0)
		return;
	if (this_ttys && this_ttys->max_col == new_col &&
	    this_ttys->max_row == new_row)
		return;

	for (i = 0; i < TTY_MAX_VDEV; i++)
		tty_resize_one(&ttys[i], new_col, new_row);
}

/* ── TTY switch ──────────────────────────────────────────────────────────── */

/*
 * tty_switch - switch the active virtual terminal to index n.
 *
 * VT switch protocol used here:
 * 1. Keyboard hotkey or VT_ACTIVATE asks to move from the current VT to n.
 * 2. The kernel always completes the visible switch immediately.
 *
 * The function is called from keyboard-triggered switching paths, so it must
 * not sleep while holding tty_switch_lock.
 */
static void tty_switch_internal(int n, int spawn_shell)
{
	int irq;
	tty_state *old_tty;
	int old_graphics;

	if (n < 1 || n > TTY_SWITCH_COUNT || n == active_tty_idx)
		return;

	spinlock_lock(&tty_switch_lock, &irq);
	old_tty = this_ttys;
	old_graphics = old_tty && old_tty->kd_mode == KD_GRAPHICS;
	if (old_graphics)
		tty_capture_graphics_locked(old_tty);
	tty_complete_switch_locked(n, spawn_shell);

	spinlock_unlock(&tty_switch_lock, irq);
}

void tty_switch(int n)
{
	tty_switch_internal(n, 1);
}

void tty_refresh_graphics(void)
{
	int irq;
	int need_queue = 0;

	spinlock_lock(&tty_switch_lock, &irq);
	if (this_ttys && this_ttys->kd_mode == KD_GRAPHICS &&
	    !tty_graphics_refresh_pending) {
		tty_graphics_refresh_pending = 1;
		need_queue = 1;
	}
	spinlock_unlock(&tty_switch_lock, irq);

	if (need_queue && !dsr_add(tty_graphics_refresh_dsr, NULL)) {
		spinlock_lock(&tty_switch_lock, &irq);
		tty_graphics_refresh_pending = 0;
		spinlock_unlock(&tty_switch_lock, irq);
	}
}

/* ── VFS file operations ─────────────────────────────────────────────────── */

static ssize_t tty_fs_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	tty_state *state = fp->f_inode->i_private;
	int nonblock = tty_file_nonblock(fp);

	if (state->released)
		return -EIO;
	if (fp->f_mode != O_RDONLY && fp->f_mode != O_RDWR)
		return -EACCES;
	if (size < 1 || !buf)
		return 0;
	if (state->termios.c_lflag & ICANON) {
		if (!state->canon_ready) {
			int r;

			if (nonblock)
				r = tty_ldisc_canon_readline(
					&state->canon, &state->termios,
					state->kb_buf, 0, 1, state->pgrp,
					tty_echo_cb, state);
			else
				r = canon_readline(state);
			if (r < 0)
				return -EINTR;
			if (r == 0)
				return nonblock ? -EAGAIN : 0;
			state->canon_ready = 1;
		}
		ssize_t n =
			(ssize_t)tty_canon_drain(&state->canon, buf, (int)size);
		if (state->canon.len == 0)
			state->canon_ready = 0;
		return n;
	}
	return raw_read(state, buf, size, !nonblock);
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

static unsigned tty_fs_poll(file *fp, unsigned events, poll_table *pt)
{
	tty_state *state = fp->f_inode->i_private;
	unsigned ready = 0;

	if ((events & FS_POLL_WRITE) && fp->f_mode != O_RDONLY)
		ready |= FS_POLL_WRITE;
	if (events & FS_POLL_READ) {
		if (fp->f_mode == O_WRONLY)
			return ready;
		if (state->termios.c_lflag & ICANON) {
			if (state->canon_ready)
				ready |= FS_POLL_READ;
		} else if (!cyb_isempty(state->kb_buf)) {
			ready |= FS_POLL_READ;
		}
	}
	if (!ready && pt && (events & FS_POLL_READ) && fp->f_mode != O_WRONLY)
		cyb_poll_read(state->kb_buf, pt);
	return ready;
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
		state->canon_ready = 0;
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
		int steal = (int)(uintptr_t)buf;
		if (!cur->user || cur->user->session_id != cur->psid)
			return -EPERM; /* must be session leader */
		if (state->pgrp && !steal)
			return -EPERM; /* already owned, not stealing */
		state->pgrp = cur->user->group_id;
		return 0;
	}
	case TIOCNOTTY: {
		task_struct *cur = CURRENT_TASK();

		if (cur->user && state->pgrp == cur->user->group_id)
			state->pgrp = 0;
		return 0;
	}
	case KDGKBTYPE:
		*(unsigned char *)buf = KB_101;
		return 0;
	case KDGETLED:
		*(int *)buf = state->kb_leds;
		return 0;
	case KDSETLED:
		state->kb_leds = (int)(uintptr_t)buf;
		return 0;
	case KDGKBMODE:
		*(int *)buf = state->kb_mode;
		return 0;
	case KDSKBMODE:
		state->kb_mode = (int)(uintptr_t)buf;
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
	case KDKBDREP: {
		struct kbd_repeat *rep = (struct kbd_repeat *)buf;

		if (rep->delay > 0)
			state->kb_repeat.delay = rep->delay;
		if (rep->period > 0)
			state->kb_repeat.period = rep->period;
		rep->delay = state->kb_repeat.delay;
		rep->period = state->kb_repeat.period;
		return 0;
	}
	case TIOCLINUX: {
		/*
		 * Linux virtual-console ioctl; subcommand is the first byte.
		 * Accept the call silently.
		 */
		return 0;
	}
	case KDSIGACCEPT:
		return 0;
	case KDADDIO:
	case KDDELIO: {
		unsigned long port = (unsigned long)(uintptr_t)buf;
		int rc;

		if (port < VGA_IO_FIRST || port > VGA_IO_LAST)
			return -EINVAL;
		rc = ps_set_ioperm(CURRENT_TASK(), port, 1, cmd == KDADDIO);
		return rc ? -ENXIO : 0;
	}
	case KDENABIO:
	case KDDISABIO: {
		int rc = ps_set_ioperm(CURRENT_TASK(), VGA_IO_FIRST,
				       VGA_IO_COUNT, cmd == KDENABIO);
		return rc ? -ENXIO : 0;
	}
	case KDGETMODE:
		*(int *)buf = state->kd_mode;
		return 0;
	case KDSETMODE: {
		int mode = (int)(uintptr_t)buf;
		task_struct *cur = CURRENT_TASK();

		if (mode != KD_TEXT && mode != KD_GRAPHICS)
			return -EINVAL;
		if (mode == KD_GRAPHICS) {
			/*
			 * Record who entered graphics mode so close/exit cleanup can
			 * restore the VT later if that same graphics owner dies.
			 */
			state->kd_mode = KD_GRAPHICS;
			state->kd_owner_pid = cur ? cur->psid : 0;
		} else {
			tty_restore_text_console_locked(state);
			if (tty_fb_text_is_visible(state)) {
				/*
				 * Returning to KD_TEXT may happen after X changed the
				 * hardware resolution. Resync geometry before we draw the
				 * saved console buffer back onto the visible screen.
				 */
				tty_sync_fb_mode_all();
				_displayed_cursor = (unsigned)state->cursor;
				fb_redraw(state->cells, state->max_col,
					  state->max_row,
					  (unsigned)state->cursor);
			}
		}
		return 0;
	}
	case VT_OPENQRY:
		*(int *)buf = tty_vt_find_free();
		return 0;
	case VT_GETMODE:
		memcpy(buf, &state->vt_mode, sizeof(state->vt_mode));
		return 0;
	case VT_SETMODE: {
		const struct vt_mode *mode = (const struct vt_mode *)buf;
		if (mode->mode != VT_AUTO && mode->mode != VT_PROCESS)
			return -EINVAL;
		memcpy(&state->vt_mode, mode, sizeof(state->vt_mode));
		return 0;
	}
	case VT_GETSTATE: {
		struct vt_stat *stat = (struct vt_stat *)buf;

		stat->v_active = (unsigned short)active_tty_idx;
		stat->v_signal = 0;
		stat->v_state = tty_vt_state_mask();
		return 0;
	}
	case VT_ACTIVATE: {
		int tty_idx = (int)(uintptr_t)buf;

		if (tty_idx < 1 || tty_idx > TTY_MAX_VDEV)
			return -EINVAL;
		/* This starts and completes a visible VT switch immediately. */
		tty_switch_internal(tty_idx, 0);
		return 0;
	}
	case VT_WAITACTIVE: {
		int tty_idx = (int)(uintptr_t)buf;

		if (tty_idx < 1 || tty_idx > TTY_MAX_VDEV)
			return -EINVAL;
		while (active_tty_idx != tty_idx)
			time_wait(10);
		return 0;
	}
	case VT_RELDISP: {
		int arg = (int)(uintptr_t)buf;

		if (arg == VT_ACKACQ)
			/* Linux uses VT_ACKACQ as a post-acquire acknowledgement.
			 * We do not gate any further state on it yet, so accept it. */
			return 0;
		if (arg == 0) {
			/* Linux allows userspace to refuse a deferred switch. We do
			 * not defer visibility anymore, so this is just an ack. */
			return 0;
		}
		if (arg == 1) {
			/* Completion acknowledgement from userspace; the visible switch
			 * already happened, so there is nothing further to do here. */
			return 0;
		}
		return -EINVAL;
	}
	case VT_DISALLOCATE:
		return 0;
	case GIO_FONT:
		memset(buf, 0, 256 * 8); /* 256 chars, 8 bytes each */
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
		return 0;
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

static int tty_fs_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	tty_state *state = node->i_private;
	s->st_atime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_dev = 0xb;
	s->st_gid = 0;
	s->st_ino = 0;
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
	task_struct *cur = CURRENT_TASK();
	unsigned owner_pid = cur ? cur->psid : 0;
	int new_count = __sync_add_and_fetch(&state->open_count, -1);

	if (new_count == 0 || (state->kd_mode == KD_GRAPHICS &&
			       state->kd_owner_pid == owner_pid)) {
		if (new_count == 0) {
			/*
			 * Last fd closed: mark the TTY released and wake up any task
			 * blocked in read().  We put an EOF sentinel (0xFF) into the
			 * keyboard buffer so that cyb_getbuf() returns immediately;
			 * canon_readline (check_eof=1) and raw_read both treat it as
			 * end-of-file and bail out.
			 */
			if (new_count == 0)
				state->released = 1;
			unsigned char eof_byte = (unsigned char)EOF;
			cyb_putbuf(state->kb_buf, &eof_byte, 1, 0, 0);
		}

		if (state->kd_mode == KD_GRAPHICS &&
		    state->kd_owner_pid == owner_pid) {
			int irq;

			spinlock_lock(&state->lock, &irq);
			tty_restore_text_console_locked(state);
			if (tty_fb_text_is_visible(state)) {
				tty_sync_fb_mode_all();
				_displayed_cursor = (unsigned)state->cursor;
				fb_redraw(state->cells, state->max_col,
					  state->max_row,
					  (unsigned)state->cursor);
			}
			spinlock_unlock(&state->lock, irq);
		}
	}

	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const file_operations tty_fops = {
	.release = tty_fs_release,
	.getattr = tty_fs_getattr,
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
	cyb_flush(state->kb_buf);
	__sync_add_and_fetch(&state->open_count, 1);

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_private = state;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &tty_fops;
	fp->f_mode = (unsigned)(flag & O_ACCMODE);
	fp->f_flag = (unsigned)flag;
	return fp;
}

/*
 * tty_cdev_open — cdev dispatch callback.
 *   major 4, minor 1-10  → tty1..tty10 (1-based; minor maps to ttys[minor-1])
 *   major 11, minor 0    → /dev/tty0   (active virtual console)
 *   major 11, minor 1    → /dev/console (system console, mapped to tty1)
 *   major 11, minor 2    → /dev/tty    (calling task's controlling terminal)
 */
static file *tty_cdev_open(super_block *sb, unsigned rdev, int flag)
{
	unsigned major = MAJOR(rdev);
	unsigned minor = MINOR(rdev);
	tty_state *state;
	task_struct *cur = CURRENT_TASK();

	if (major == TTY_VC_MAJOR) {
		/* /dev/ttyN uses 1-based minor: tty1→ttys[0] (active), tty2→ttys[1], … */
		if (minor < 1 || minor - 1 >= TTY_MAX_VDEV)
			return NULL;
		state = &ttys[minor - 1];
	} else if (major != TTY_AUX_MAJOR) {
		return NULL;
	} else if (minor == 0) {
		state = &ttys[active_tty_idx - 1];
	} else if (minor == 1) {
		state = &ttys[DEFAULT_TTY - 1];
	} else if (minor == 2) {
		state = tty_find_controlling(cur);
		if (!state) {
			file *fp = pty_open_controlling(cur, flag);

			if (fp)
				return fp;
			fp = ptmx_open_controlling(cur, flag);
			if (fp)
				return fp;
			return NULL;
		}
	} else {
		return NULL;
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
		unsigned sz = t->max_row * t->max_col;
		unsigned j;

		t->kb_buf = cyb_create(1);
		t->cells = (tty_cell_t *)zalloc(sz * sizeof(tty_cell_t));
		t->alt_cells = (tty_cell_t *)zalloc(sz * sizeof(tty_cell_t));
		for (j = 0; j < sz; j++) {
			t->cells[j].ch = ' ';
			t->cells[j].fg = VGA_COLOR_WHITE;
			t->cells[j].bg = VGA_COLOR_BLACK;
		}
		if (i > 0)
			t->parent = ps_find_process(1);
	}

	this_ttys = &ttys[DEFAULT_TTY - 1];
}

static void tty_dev_register(super_block *dev_sb)
{
	char path[16];
	int i;

	printk("dev: registered /dev/tty[1-%d]\n", TTY_MAX_VDEV);
	/* major 4: tty1..tty10 (1-based minors matching tty_idx) */
	cdev_register(S_IFCHR, TTY_VC_MAJOR, 1, TTY_MAX_VDEV, tty_cdev_open);
	for (i = 1; i <= TTY_MAX_VDEV; i++) {
		sprintf(path, "/tty%x", i);
		vfs_mknod(dev_sb, path, S_IFCHR | 0620, MKDEV(TTY_VC_MAJOR, i));
	}

	printk("dev: registered /dev/tty0\n");
	printk("dev: registered /dev/console\n");
	printk("dev: registered /dev/tty\n");
	/* major 11: /dev/tty0, /dev/console, /dev/tty */
	cdev_register(S_IFCHR, TTY_AUX_MAJOR, 0, 3, tty_cdev_open);
	vfs_mknod(dev_sb, "/tty0", S_IFCHR | 0620, MKDEV(TTY_AUX_MAJOR, 0));
	vfs_mknod(dev_sb, "/console", S_IFCHR | 0600, MKDEV(TTY_AUX_MAJOR, 1));
	vfs_mknod(dev_sb, "/tty", S_IFCHR | 0620, MKDEV(TTY_AUX_MAJOR, 2));
}

KERNEL_INIT(0, tty_fs_init);
DEV_INIT(tty_dev_register);
