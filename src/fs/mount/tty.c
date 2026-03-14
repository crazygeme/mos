/*
 * src/fs/mount/tty.c - TTY driver: VGA/framebuffer output, keyboard input,
 * ANSI escape processing, termios, and /dev/tty filesystem device.
 *
 * Merges hw/tty.c + fs/mount/console.c + fs/mount/kbchar.c into a single
 * self-contained driver.  All mutable state lives in the private tty_state
 * struct; this_tty points to the single statically-allocated instance.
 *
 * Public kernel interface (see include/hw/tty.h):
 *   tty_init()          - early VGA/FB init, called before the VM is up
 *   tty_default_emit_unsafe(c, ctx)    - single-char output callback for kprint
 *   tty_lock_acquire()  - acquire the TTY spinlock (for printk batching)
 *   tty_lock_release()  - release the TTY spinlock
 *   tty_default_clear()  - clear screen under the TTY lock (for exec)
 */

#include <int/int.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/mount.h>
#include <fs/ioctl.h>
#include <hw/tty.h>
#include <hw/vga.h>
#include <hw/keyboard.h>
#include <hw/time.h>
#include <lib/port.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <unistd.h>
#include <errno.h>
#include <macro.h>
#include <ext4.h>

#define CANON_BUF_SIZE 256

/* ── Private TTY state ───────────────────────────────────────────────────── */

typedef struct {
	/* VGA/framebuffer */
	char *vidptr;
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
// FIXME(Ender): support multiple TTYs switch with ioctl(TIOCSETD) and friends
static tty_state *this_ttys = &ttys[0];

/* ── Convenience macros ──────────────────────────────────────────────────── */

#define MAX_ROW (state->max_row)
#define MAX_COL (state->max_col)
#define MAX_CHARS ((int)(MAX_ROW * MAX_COL))
#define CUR_ROW (state->cursor / (int)MAX_COL)
#define CUR_COL (state->cursor % (int)MAX_COL)

/* ── VGA/framebuffer primitives ──────────────────────────────────────────── */

static void vga_putchar(tty_state *state, int x, int y, char c)
{
	if (x < 0 || x >= (int)MAX_ROW || y < 0 || y >= (int)MAX_COL)
		return;
	if (!fb_is_available()) {
		int pos = x * (int)MAX_COL + y;
		state->vidptr[pos * 2] = c;
	} else {
		fb_putchar(y, x, c);
	}
}

static void vga_clear_row(tty_state *state, int row)
{
	int col;
	for (col = 0; col < (int)MAX_COL; col++)
		vga_putchar(state, row, col, ' ');
}

static void tty_do_clear(tty_state *state)
{
	if (!fb_is_available()) {
		int row;
		char *src = state->vidptr;
		unsigned len = MAX_COL * 2;
		vga_clear_row(state, 0);
		for (row = 1; row < (int)MAX_ROW; row++)
			memcpy(src + row * len, src, len);
	} else {
		fb_clear_screen();
	}
}

static void tty_roll_line(tty_state *state)
{
	if (!fb_is_available()) {
		char *dst = state->vidptr;
		char *src = dst + MAX_COL * 2;
		memmove(dst, src, MAX_COL * (MAX_ROW - 1) * 2);
		vga_clear_row(state, MAX_ROW - 1);
	} else {
		fb_scroll_line();
	}
}

static void tty_hw_cursor(unsigned pos)
{
	if (!fb_is_available()) {
		unsigned short cp = (unsigned short)pos;
		port_write_word(0x3d4, 0x0e | (cp & 0xff00));
		port_write_word(0x3d4, 0x0f | (cp << 8));
	} else {
		fb_update_cursor(pos);
	}
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
	tty_hw_cursor((unsigned)state->cursor);
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
	tty_hw_cursor((unsigned)state->cursor);
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

/* Read one complete line into canon_buf. Returns 1 on line, 0 on EOF. */
static int canon_readline(tty_state *state)
{
	const struct termios *tc = &state->termios;
	while (1) {
		int ch = input_translate(kb_buf_get(), tc->c_iflag);
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
		if ((unsigned)n >= vmin && !kb_can_read())
			break;
		int ch = input_translate(kb_buf_get(), tc->c_iflag);
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
	for (int i = 0; i < TTY_MAX_VDEV; i++) {
		tty_state *t = &ttys[i];
		t->vidptr = (char *)0xC00b8000;
		if (fb_is_available()) {
			fb_get_char_dims(&t->max_col, &t->max_row);
		} else {
			t->max_row = 25;
			t->max_col = 80;
		}
		for (j = 0; j < (int)t->max_row; j++)
			for (k = 0; k < (int)t->max_col; k++)
				vga_putchar(t, j, k, ' ');
		t->cursor = 0;
		spinlock_init(&t->lock);
		t->termios = default_termios;
	}
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
	tty_hw_cursor((unsigned)state->cursor);
	fp->f_pos = state->cursor;
	return fp->f_pos;
}

static int tty_fs_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_WRITE)
		return (fp->f_mode == O_RDONLY) ? -1 : 0;
	if (type == FS_POLL_READ)
		return (fp->f_mode == O_WRONLY) ? -1 : 0;
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

/* ── Filesystem registration ─────────────────────────────────────────────── */

static super_operations tty_sops = {
	.open_root = tty_open_root,
};

static void tty_fs_init(void)
{
	char mount_path[16];
	super_block *sb;
	task_struct *cur = CURRENT_TASK();

	for (int i = 0; i < TTY_MAX_VDEV; i++) {
		sb = sget(&tty_sops);
		sb->s_fs_info = &ttys[i];
		sprintf(mount_path, "/dev/tty%d", i);
		vfs_mount(cur->root, mount_path, sb);
	}

	sb = sget(&tty_sops);
	sb->s_fs_info = &ttys[0];
	vfs_mount(cur->root, "/dev/tty", sb);
}

KERNEL_INIT(4, tty_fs_init);
