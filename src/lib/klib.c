/*
 * src/lib/klib.c - Kernel library: initialisation, TTY output, ANSI escape
 * processing, cursor management, and 64-bit arithmetic helpers.
 *
 * Companion modules:
 *   kmalloc.c  - heap allocator (malloc/free/calloc)
 *   kprint.c   - formatted output (printf/printk/klog)
 *   kstring.c  - string/memory/character functions
 */

#include <macro.h>
#include <klib.h>
#include <tty.h>
#include <time.h>
#include <lock.h>

/* ── Shared locks (also used by kprint.c) ────────────────────────────────── */

spinlock_t tty_lock;

/* ── TTY cursor state ────────────────────────────────────────────────────── */

static int cursor = 0;

#define CUR_ROW (cursor / TTY_MAX_COL)
#define CUR_COL (cursor % TTY_MAX_COL)

/* ── Forward declarations ────────────────────────────────────────────────── */

/* Used by klib_putchar (tab expansion) before its definition. */
static void klib_putchar_update_cursor(char c, void *ctx);

/* Defined in kmalloc.c */
extern void kmalloc_init(void);

/* Defined in kprint.c */
extern void klog_init(void);

/* ── ANSI escape state ───────────────────────────────────────────────────── */

static int ansi_flag = 0;
static char ansi_buf[10];
static int ansi_idx = 0;

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

/* Forward declaration */
static void klib_cursor_forward(int new_pos);

/*
 * Process one character of an ANSI escape sequence.
 * Called for every character after ESC '[' has been seen.
 */
static void ansi_feed(char c)
{
	char *arg = ansi_buf + 1;
	int val, row, col, new_pos;

	switch (c) {
	case 'm': /* SGR - colour/attribute (stub) */
		ansi_end();
		return;
	case 'A': /* cursor up n */
		val = atoi(arg);
		row = CUR_ROW - val;
		if (row < 0)
			row = 0;
		klib_cursor_forward(ROW_COL_TO_CUR(row, CUR_COL));
		ansi_end();
		return;
	case 'B': /* cursor down n */
		val = atoi(arg);
		row = CUR_ROW + val;
		if (row >= TTY_MAX_ROW)
			row = TTY_MAX_ROW - 1;
		klib_cursor_forward(ROW_COL_TO_CUR(row, CUR_COL));
		ansi_end();
		return;
	case 'C': /* cursor right n */
		val = atoi(arg);
		col = CUR_COL + val;
		if (col >= TTY_MAX_COL)
			col = TTY_MAX_COL - 1;
		klib_cursor_forward(ROW_COL_TO_CUR(CUR_ROW, col));
		ansi_end();
		return;
	case 'D': /* cursor left n */
		val = atoi(arg);
		col = CUR_COL - val;
		if (col < 0)
			col = 0;
		klib_cursor_forward(ROW_COL_TO_CUR(CUR_ROW, col));
		ansi_end();
		return;
	case 'H': { /* set cursor position row;col */
		char *str_col = strchr(arg, ';');

		if (!str_col)
			break;
		*str_col++ = '\0';
		row = atoi(arg) - 1;
		col = atoi(str_col) - 1;
		if (row < 0)
			row = 0;
		if (row >= TTY_MAX_ROW)
			row = TTY_MAX_ROW - 1;
		if (col < 0)
			col = 0;
		if (col >= TTY_MAX_COL)
			col = TTY_MAX_COL - 1;
		klib_cursor_forward(ROW_COL_TO_CUR(row, col));
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

	/* Accumulate non-terminator characters; terminate on any letter. */
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
		ansi_end();
	} else if (ansi_idx < (int)sizeof(ansi_buf) - 1) {
		ansi_buf[ansi_idx++] = c;
	}
}

/* ── Low-level putchar ───────────────────────────────────────────────────── */

/*
 * Write one character to the TTY, returning the new cursor position.
 * Does NOT update the hardware cursor; the caller decides when to flush.
 */
static int klib_putchar(char c, void *ctx)
{
	int new_pos;

	(void)ctx;

	if (ansi_flag) {
		ansi_feed(c);
		return cursor;
	}

	if (c == '\n') {
		new_pos = ROW_COL_TO_CUR((CUR_ROW + 1), 0);
	} else if (c == '\t') {
		int spaces = 8 - (CUR_COL % 8);
		int i;

		if (spaces == 0)
			spaces = 8;
		for (i = 0; i < spaces; i++)
			klib_putchar_update_cursor(' ', ctx);
		return cursor;
	} else if (c == '\r') {
		new_pos = ROW_COL_TO_CUR(CUR_ROW, 0);
	} else if (c == '\b') {
		new_pos = cursor - 1;
	} else if ((unsigned char)c == 0x0c) { /* form-feed: clear screen */
		tty_clear();
		return cursor;
	} else if (isprint(c)) {
		tty_putchar(CUR_ROW, CUR_COL, c);
		new_pos = cursor + 1;
	} else if ((unsigned char)c == 0x1b) { /* ESC: begin ANSI sequence */
		ansi_begin();
		return cursor;
	} else {
		return -1;
	}

	return new_pos;
}

static void klib_cursor_forward(int new_pos)
{
	cursor = new_pos;
	/* Scroll if past the last character position. */
	while (cursor >= TTY_MAX_CHARS) {
		tty_roll_one_line();
		cursor -= TTY_MAX_COL;
	}
	tty_movecurse((unsigned)cursor);
}

/* ── Public TTY helpers ──────────────────────────────────────────────────── */

void klib_putchar_update_cursor(char c, void *ctx)
{
	int new_pos = klib_putchar(c, ctx);

	klib_cursor_forward(new_pos);
}

void klib_print(char *str, void *ctx)
{
	if (!str || !*str)
		return;
	while (*str)
		klib_putchar_update_cursor(*str++, ctx);
}

void klib_update_cursor(int pos)
{
	if (pos >= 0)
		cursor = pos;
}

void klib_flush_cursor(void)
{
	klib_cursor_forward(cursor);
}

int klib_get_pos(void)
{
	return cursor;
}

void klib_clear(void)
{
	spinlock_lock(&tty_lock);
	tty_clear();
	cursor = 0;
	spinlock_unlock(&tty_lock);
}

/*
 * tty_write - write raw bytes to the TTY under the TTY lock.
 * Used by the VFS/devfs TTY driver.
 */
void tty_write(const char *buf, unsigned len)
{
	unsigned i;

	spinlock_lock(&tty_lock);
	for (i = 0; i < len; i++) {
		if (cursor >= TTY_MAX_CHARS)
			klib_flush_cursor();
		klib_update_cursor(klib_putchar(buf[i], NULL));
	}
	klib_flush_cursor();
	spinlock_unlock(&tty_lock);
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

void klib_init(void)
{
	tty_init();
	cursor = 0;
	spinlock_init(&tty_lock);
	kmalloc_init();
}

/* ── 64-bit arithmetic helpers (libgcc replacements) ─────────────────────── */

typedef unsigned long long uint64_t;

uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t *rem_p)
{
	uint64_t quot = 0, qbit = 1;

	if (den == 0) {
		/* Intentional divide by zero to match libgcc behaviour. */
		return 1 / ((unsigned)den);
	}

	while ((int64_t)den >= 0) {
		den <<= 1;
		qbit <<= 1;
	}

	while (qbit) {
		if (den <= num) {
			num -= den;
			quot += qbit;
		}
		den >>= 1;
		qbit >>= 1;
	}

	if (rem_p)
		*rem_p = num;
	return quot;
}

uint64_t __umoddi3(uint64_t num, uint64_t den)
{
	uint64_t v;

	(void)__udivmoddi4(num, den, &v);
	return v;
}

uint64_t __udivdi3(uint64_t num, uint64_t den)
{
	return __udivmoddi4(num, den, NULL);
}

KERNEL_INIT(0, klog_init);
