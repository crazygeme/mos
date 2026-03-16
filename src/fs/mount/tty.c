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

#include <int/int.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
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
#include <ext4.h>

#define CANON_BUF_SIZE 256
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
	char canon_buf[CANON_BUF_SIZE];
	int canon_len;
	/* per-TTY keyboard input queue */
	cy_buf *kb_buf;
	/* screen text buffer for inactive TTYs (also used on switch-away save) */
	char *saved_text;
	/* TTY index (0..TTY_MAX_VDEV-1) */
	int tty_idx;
	/* PID of the bash process running on this TTY (0 = none) */
	unsigned bash_pid;
	/* Parent task for this TTY, used for setting root and waitpid */
	task_struct *parent;
} tty_state;

static const struct termios default_termios = {
	.c_iflag = ICRNL,
	.c_oflag = OPOST | ONLCR,
	.c_cflag = B38400 | CS8,
	.c_lflag = IXON | ISIG | ICANON | ECHO | ECHOE | ECHOCTL | ECHOKE,
	.c_line = 0,
	.c_cc = INIT_C_CC,
};

static tty_state ttys[TTY_MAX_VDEV];
static tty_state *this_ttys = &ttys[0];
static int active_tty_idx = 0;

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
		fb_putchar(y, x, c);
	} else {
		/* Inactive TTY: write to saved_text only */
		state->saved_text[x * (int)MAX_COL + y] = c;
	}
}

static void tty_do_clear(tty_state *state)
{
	if (state->tty_idx == active_tty_idx) {
		fb_clear_screen();
	} else {
		memset(state->saved_text, 0,
		       (unsigned)(MAX_ROW * (int)MAX_COL));
	}
}

static void tty_roll_line(tty_state *state)
{
	if (state->tty_idx == active_tty_idx) {
		fb_scroll_line();
	} else {
		/* Scroll saved_text up one row */
		unsigned row_bytes = (unsigned)MAX_COL;
		memmove(state->saved_text, state->saved_text + row_bytes,
			row_bytes * (unsigned)(MAX_ROW - 1));
		memset(state->saved_text + row_bytes * (unsigned)(MAX_ROW - 1),
		       0, row_bytes);
	}
}

/*
 * tty_hw_cursor - update the hardware cursor register (or framebuffer cursor).
 * Skipped for inactive TTYs — they track cursor in state->cursor only.
 */
static void tty_hw_cursor(tty_state *state, unsigned pos)
{
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
	case 'm': /* SGR - colour/attribute (stub) */
		ansi_end(state);
		return;
	case 'A': /* cursor up n */
		val = atoi(arg);
		row = CUR_ROW - val;
		if (row < 0)
			row = 0;
		cursor_set(state, row * (int)MAX_COL + CUR_COL);
		ansi_end(state);
		return;
	case 'B': /* cursor down n */
		val = atoi(arg);
		row = CUR_ROW + val;
		if (row >= (int)MAX_ROW)
			row = (int)MAX_ROW - 1;
		cursor_set(state, row * (int)MAX_COL + CUR_COL);
		ansi_end(state);
		return;
	case 'C': /* cursor right n */
		val = atoi(arg);
		col = CUR_COL + val;
		if (col >= (int)MAX_COL)
			col = (int)MAX_COL - 1;
		cursor_set(state, CUR_ROW * (int)MAX_COL + col);
		ansi_end(state);
		return;
	case 'D': /* cursor left n */
		val = atoi(arg);
		col = CUR_COL - val;
		if (col < 0)
			col = 0;
		cursor_set(state, CUR_ROW * (int)MAX_COL + col);
		ansi_end(state);
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
	case 'J': /* erase display */
		tty_do_clear(state);
		ansi_end(state);
		return;
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

static int input_translate(unsigned char c, tcflag_t iflag)
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

static int isig_check(tty_state *state, unsigned char c)
{
	const struct termios *tc = &state->termios;
	if (!(tc->c_lflag & ISIG))
		return 0;
	return (tc->c_cc[VINTR] && c == tc->c_cc[VINTR]) ||
	       (tc->c_cc[VQUIT] && c == tc->c_cc[VQUIT]) ||
	       (tc->c_cc[VSUSP] && c == tc->c_cc[VSUSP]);
}

static void canon_erase(tty_state *state)
{
	const struct termios *tc = &state->termios;
	if (state->canon_len == 0)
		return;
	state->canon_len--;
	if (!(tc->c_lflag & ECHO))
		return;
	if (tc->c_lflag & ECHOE)
		tty_do_write(state, "\b \b", 3);
	else
		tty_do_write(state, (const char *)&tc->c_cc[VERASE], 1);
}

static void canon_kill(tty_state *state)
{
	const struct termios *tc = &state->termios;
	if (tc->c_lflag & ECHO) {
		if (tc->c_lflag & ECHOK) {
			int i;
			for (i = 0; i < state->canon_len; i++)
				tty_do_write(state, "\b \b", 3);
		} else {
			tty_do_write(state, (const char *)&tc->c_cc[VKILL], 1);
			tty_do_write(state, "\n", 1);
		}
	}
	state->canon_len = 0;
}

static void canon_append(tty_state *state, unsigned char c)
{
	const struct termios *tc = &state->termios;
	if (state->canon_len >= CANON_BUF_SIZE - 1)
		return;
	state->canon_buf[state->canon_len++] = (char)c;
	if (tc->c_lflag & ECHO)
		tty_do_write(state, &state->canon_buf[state->canon_len - 1], 1);
	else if (c == '\n' && (tc->c_lflag & ECHONL))
		tty_do_write(state, "\n", 1);
}

static int is_eol(tty_state *state, unsigned char c)
{
	const struct termios *tc = &state->termios;
	return c == '\n' || (tc->c_cc[VEOL] && c == tc->c_cc[VEOL]) ||
	       (tc->c_cc[VEOL2] && c == tc->c_cc[VEOL2]);
}

/* Read one complete line into canon_buf. Returns 1 on line, 0 on EOF.
 * Blocks on this TTY's per-TTY keyboard buffer. */
static int canon_readline(tty_state *state)
{
	const struct termios *tc = &state->termios;
	while (1) {
		int ch = input_translate(cyb_getc(state->kb_buf), tc->c_iflag);
		if (ch < 0)
			continue;
		unsigned char c = (unsigned char)ch;
		if (isig_check(state, c))
			continue;
		if (tc->c_cc[VERASE] && c == tc->c_cc[VERASE]) {
			canon_erase(state);
			continue;
		}
		if (tc->c_cc[VKILL] && c == tc->c_cc[VKILL]) {
			canon_kill(state);
			continue;
		}
		if (tc->c_cc[VEOF] && c == tc->c_cc[VEOF])
			return state->canon_len > 0;
		canon_append(state, c);
		if (is_eol(state, c))
			return 1;
	}
}

/* Drain up to size bytes from canon_buf into dst. */
static ssize_t canon_drain(tty_state *state, char *dst, size_t size)
{
	ssize_t n = (ssize_t)(size < (size_t)state->canon_len ?
				      size :
				      (size_t)state->canon_len);
	memcpy(dst, state->canon_buf, (unsigned)n);
	state->canon_len -= (int)n;
	if (state->canon_len > 0)
		memmove(state->canon_buf, state->canon_buf + n,
			(unsigned)state->canon_len);
	return n;
}

/* Raw mode read: block until VMIN chars are available, then drain. */
static ssize_t raw_read(tty_state *state, char *dst, size_t size)
{
	const struct termios *tc = &state->termios;
	unsigned vmin = tc->c_cc[VMIN];
	ssize_t n = 0;
	while ((size_t)n < size) {
		if ((unsigned)n >= vmin && cyb_isempty(state->kb_buf))
			break;
		int ch = input_translate(cyb_getc(state->kb_buf), tc->c_iflag);
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
		t->termios = default_termios;
		t->tty_idx = i;
		t->bash_pid = 0;
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

	cyb_putc(t->kb_buf, c);
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
	char *argv[] = { "/bin/bash", NULL };
	char *envp[] = { "PATH=/bin:/usr/bin:/sbin", NULL };
	struct stat st;
	task_struct *cur = CURRENT_TASK();

	sprintf(tty_path, "/dev/tty%d", state->tty_idx);

	/* Wire this task to the root filesystem. */
	cur->root = state->parent->root;
	sb_get(cur->root);

	/* set parent of current so that wait can work */
	cur->parent = state->parent;

	/* Set working directory. */
	strcpy(cur->cwd, "/");

	/* Set up TSS esp0 for user-mode entry. */
	ps_update_tss((unsigned)cur + PAGE_SIZE);

	/* Open stdin (0), stdout (1), stderr (2) on this TTY. */
	fs_open(tty_path, O_RDONLY, NULL);
	fs_open(tty_path, O_WRONLY, NULL);
	fs_open(tty_path, O_WRONLY, NULL);

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
	spinlock_unlock(&this_ttys->lock);

	/* Flip active TTY — visible to other CPUs only after this point. */
	active_tty_idx = n;
	this_ttys = except_tty;

	/* Restore new TTY's cached text to the framebuffer. */
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
	if (fp->f_mode != O_RDONLY && fp->f_mode != O_RDWR)
		return -EACCES;
	if (size < 1 || !buf)
		return 0;
	if (state->termios.c_lflag & ICANON) {
		if (state->canon_len == 0 && !canon_readline(state))
			return 0;
		return canon_drain(state, buf, size);
	}
	return raw_read(state, buf, size);
}

static ssize_t tty_fs_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	tty_state *state = fp->f_inode->i_private;
	if (fp->f_mode != O_WRONLY && fp->f_mode != O_RDWR)
		return -EACCES;
	if (size < 1 || !buf)
		return 0;
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
			return (state->canon_len > 0) ? 0 : -1;
		return cyb_isempty(state->kb_buf) ? -1 : 0;
	}
	return -1;
}

static int tty_fs_ioctl(file *fp, unsigned cmd, void *buf)
{
	tty_state *state = fp->f_inode->i_private;
	switch (cmd) {
	case TCGETS:
		memcpy(buf, &state->termios, sizeof(state->termios));
		return 0;
	case TCSETS:
	case TCSETSW:
		memcpy(&state->termios, buf, sizeof(state->termios));
		return 0;
	case TIOCGWINSZ: {
		struct winsize *ws = (struct winsize *)buf;
		ws->ws_row = (unsigned short)MAX_ROW;
		ws->ws_col = (unsigned short)MAX_COL;
		ws->ws_xpixel = ws->ws_ypixel = 0;
		return 0;
	}
	case TIOCGPGRP:
		*(unsigned *)buf = state->pgrp;
		return 0;
	case TIOCSPGRP:
		state->pgrp = *(unsigned *)buf;
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
	s->st_rdev = 8004;
	s->st_size = (loff_t)(MAX_CHARS - state->cursor);
	return 0;
}

static const inode_operations tty_iops = {
	.getattr = tty_fs_getattr,
};

static const file_operations tty_fops = {
	.read = tty_fs_read,
	.write = tty_fs_write,
	.llseek = tty_fs_llseek,
	.poll = tty_fs_poll,
	.ioctl = tty_fs_ioctl,
};

static file *tty_open_root(super_block *sb)
{
	inode *node = calloc(1, sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
		       S_IROTH | S_IWOTH;
	node->i_op = &tty_iops;
	node->i_private = sb->s_fs_info;

	file *fp = calloc(1, sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &tty_fops;
	/* f_mode is set by fs_open() after open_root returns */
	return fp;
}

static void tty_sops_release(super_block *sb)
{
	tty_state *state = sb->s_fs_info;
	if (state->kb_buf)
		cyb_destroy(state->kb_buf);
	if (state->saved_text)
		free(state->saved_text);
	free(sb);
}

/* ── Filesystem registration ─────────────────────────────────────────────── */

static super_operations tty_sops = {
	.open_root = tty_open_root,
	.release = tty_sops_release,
};

static void tty_fs_init(void)
{
	char mount_path[16];
	super_block *sb;
	task_struct *cur = CURRENT_TASK();

	for (int i = 0; i < TTY_MAX_VDEV; i++) {
		tty_state *t = &ttys[i];
		/* Per-TTY keyboard input queue */
		t->kb_buf = cyb_create();
		/* Screen text buffer for save/restore on TTY switch */
		t->saved_text = (char *)calloc(1, t->max_row * t->max_col);
		/* 
		 * All those attached bash should be managed by process 1 
		 * (/sbin/init) which will call wait on all orphen processes
		 */
		if (i > 0)
			t->parent = ps_find_process(1);
		sb = sget(&tty_sops);
		sb->s_fs_info = t;
		sprintf(mount_path, "/dev/tty%d", i);
		vfs_mount(cur->root, mount_path, sb);
	}

	sb = sget(&tty_sops);
	sb->s_fs_info = &ttys[0];
	vfs_mount(cur->root, "/dev/tty", sb);
	sb_get(sb);
	vfs_mount(cur->root, "/dev/console", sb);
}

KERNEL_INIT(4, tty_fs_init);
