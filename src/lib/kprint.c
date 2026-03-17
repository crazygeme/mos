/*
 * src/lib/kprint.c - Kernel formatted output (printf / printk / klog family).
 *
 * All output paths funnel through vprintf(), which accumulates characters in
 * a stack-local buffer and flushes to the caller-supplied _putstr callback.
 * Using a stack-local buffer (instead of a static global) makes vprintf
 * re-entrant across different call sites.
 *
 * Format specifiers supported:
 *   %d  signed decimal       %u  unsigned decimal
 *   %x  hex (0x prefix)      %p  same as %x
 *   %s  string               %c  character
 *   %b  two-digit hex byte   %h  human-readable size (e.g. "1.2M")
 *   %%  literal percent
 *
 * Flags and width (subset of C99):
 *   -    left-align within field width
 *   0    zero-pad instead of space-pad (numeric types)
 *   N    minimum field width (e.g. %5d, %02u, %-10s)
 */

#include <ps/ps.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <hw/serial.h>
#include <hw/tty.h>
#include <hw/time.h>
#include <fs/syslog.h>

/* ── Locks ───────────────────────────────────────────────────────────────── */

static int klog_inited = 0;
static mutex_t klog_lock;

/* ── klog backend ────────────────────────────────────────────────────────── */
void klog_init(void)
{
	klog_inited = 1;
	mutex_init(&klog_lock);
}

static void klog_write(char c, void *ctx)
{
	if (!klog_inited)
		return;
	if (isprint(c))
		serial_putc(c);
}

static void klog_writestr(char *str, void *ctx)
{
	if (!klog_inited || !str || !*str)
		return;
	syslog_write(str, strlen(str));
	while (*str)
		klog_write(*str++, ctx);
}

void klog_close(void)
{
	if (klog_inited)
		serial_flush();
}

/* ── sprintf helpers ─────────────────────────────────────────────────────── */

static void putstr_to_str(char *str, void *ctx)
{
	strcat(ctx, str);
}

/* ── Human-readable size (%h specifier) ─────────────────────────────────── */

typedef void (*fputstr)(char *str, void *ctx);

static void print_human_size(fputstr _putstr, unsigned sz, void *ctx)
{
	static const char *units[] = { "", "k", "M", "G", NULL };
	const char **up = units;
	unsigned frac = 0;
	char buf[32];
	int n = 0;
	char *s, *f;

	if (sz == 1) {
		_putstr("1", ctx);
		return;
	}

	while (sz >= 1024 && up[1]) {
		frac = (sz % 1024) / 100;
		sz /= 1024;
		up++;
	}

	s = itoa((int)sz, 10, 0);
	f = itoa((int)frac, 10, 0);

	/* Build "sz.frac unit" in buf without calling sprintf */
	char *p;

	for (p = s; *p; p++)
		buf[n++] = *p;
	buf[n++] = '.';
	for (p = f; *p; p++)
		buf[n++] = *p;
	for (p = (char *)*up; *p; p++)
		buf[n++] = *p;
	buf[n] = '\0';

	free(s);
	free(f);
	_putstr(buf, ctx);
}

/* ── Core formatter ──────────────────────────────────────────────────────── */

#define VBUF_SZ 256

/*
 * kvformat - single internal implementation of formatted output.
 *
 * Accumulates characters in a stack-local buffer (re-entrant; no global state)
 * and flushes to the caller-supplied _putstr callback.  All public print
 * functions delegate here; none duplicate the format loop.
 */
static void kvformat(fputstr _putstr, const char *fmt, va_list ap, void *ctx)
{
	char buf[VBUF_SZ];
	int pos = 0;
	const char *p = fmt;

#define FLUSH()                            \
	do {                               \
		buf[pos] = '\0';           \
		if (pos)                   \
			_putstr(buf, ctx); \
		pos = 0;                   \
	} while (0)

#define EMIT(c)                         \
	do {                            \
		buf[pos++] = (c);       \
		if (pos >= VBUF_SZ - 1) \
			FLUSH();        \
	} while (0)

#define EMITS(s)                              \
	do {                                  \
		const char *_s = (s);         \
		if (!_s)                      \
			_putstr(buf, "NULL"); \
		else                          \
			while (*_s)           \
				EMIT(*_s++);  \
	} while (0)

/*
 * EMIT_PADDED(str, len, width, fill, left_align)
 *
 * Emit the string `str` of known byte length `len`, padding to `width`
 * columns.  `fill` is the pad character (' ' or '0').  When `left_align`
 * is set the padding goes on the right; otherwise on the left.
 *
 * For zero-padding of signed numbers the sign character must be emitted
 * before the pad — the caller handles that by stripping the '-' and
 * passing it separately.
 */
#define EMIT_PADDED(str, len, width, fill, left_align) \
	do {                                           \
		const char *_ep = (str);               \
		int _elen = (len);                     \
		int _epad = (width) - _elen;           \
		if (!(left_align))                     \
			for (; _epad > 0; _epad--)     \
				EMIT(fill);            \
		for (; *_ep; _ep++)                    \
			EMIT(*_ep);                    \
		if (left_align)                        \
			for (; _epad > 0; _epad--)     \
				EMIT(' ');             \
	} while (0)

	while (*p) {
		char cur = *p++;

		if (cur != '%') {
			EMIT(cur);
			continue;
		}

		/* ---- Parse flags ---- */
		int flag_zero = 0, flag_left = 0;
		for (;;) {
			if (*p == '-') {
				flag_left = 1;
				p++;
			} else if (*p == '0') {
				flag_zero = 1;
				p++;
			} else
				break;
		}

		/* ---- Parse width ---- */
		int width = 0;
		while (*p >= '1' && *p <= '9')
			width = width * 10 + (*p++ - '0');

		/* left-align overrides zero-fill */
		char fill = (flag_zero && !flag_left) ? '0' : ' ';

		cur = *p++;
		switch (cur) {
		case '%':
			EMIT('%');
			break;
		case 'd': {
			int v = va_arg(ap, int);
			char *s = itoa(v, 10, 1);
			char *num = s;
			int neg = (v < 0 && flag_zero);

			/* For zero-pad signed: emit '-' first, then pad digits */
			if (neg) {
				EMIT('-');
				num++;
			}
			EMIT_PADDED(num, (int)strlen(num), width - neg, fill,
				    flag_left);
			free(s);
			break;
		}
		case 'u': {
			unsigned v = va_arg(ap, unsigned);
			char *s = itoa((int)v, 10, 0);

			EMIT_PADDED(s, (int)strlen(s), width, fill, flag_left);
			free(s);
			break;
		}
		case 'x':
		case 'p': {
			unsigned v = va_arg(ap, unsigned);
			char *s = itoa((int)v, 16, 0);

			EMIT_PADDED(s, (int)strlen(s), width, fill, flag_left);
			free(s);
			break;
		}
		case 's': {
			char *v = va_arg(ap, char *);

			if (!v)
				v = "(null)";
			EMIT_PADDED(v, (int)strlen(v), width, ' ', flag_left);
			break;
		}
		case 'c': {
			char v = (char)va_arg(ap, int);

			if (width > 1 && !flag_left)
				for (int _i = 1; _i < width; _i++)
					EMIT(' ');
			EMIT(v);
			if (width > 1 && flag_left)
				for (int _i = 1; _i < width; _i++)
					EMIT(' ');
			break;
		}
		case 'b': {
			unsigned char v = (unsigned char)va_arg(ap, unsigned);
			char *s = itoa(v, 16, 0);
			/* Skip "0x" prefix produced by itoa; zero-pad to 2 hex digits */
			char *hex = s + 2;

			if (strlen(hex) == 1)
				EMIT('0');
			EMITS(hex);
			free(s);
			break;
		}
		case 'h': {
			unsigned v = va_arg(ap, unsigned);

			FLUSH();
			print_human_size(_putstr, v, ctx);
			break;
		}
		default:
			EMIT('?');
			break;
		}
	}

#undef EMIT_PADDED

	FLUSH();

#undef FLUSH
#undef EMIT
#undef EMITS
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* vprintf/vsprintf are the standard variadic-list entrypoints; all va_list
 * callers use these so there is no duplication of the format loop. */

static void tty_print(char *str, void *ctx)
{
	if (!str || !*str)
		return;
	while (*str) {
		if (*str == '\n') {
			tty_default_emit_unsafe('\r', ctx);
			tty_default_emit_unsafe('\n', ctx);
		} else
			tty_default_emit_unsafe(*str, ctx);

		str++;
	}
}

void vprintf(const char *fmt, va_list ap)
{
	kvformat(tty_print, fmt, ap, NULL);
}

void vsprintf(char *buf, const char *fmt, va_list ap)
{
	buf[0] = '\0';
	kvformat(putstr_to_str, fmt, ap, buf);
}

/* printf/sprintf are the variadic shims that set up a va_list and delegate. */

void printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);
	va_end(ap);
}

/*
 * printk - timestamped kernel log to the TTY.
 * Format: [seconds.milliseconds] message
 * printf() is safe to call under tty_lock because it does not acquire it.
 */
void printk(const char *fmt, ...)
{
	va_list ap;

	tty_lock_acquire();
	printf("[%d]: ", CURRENT_TASK()->psid);

	va_start(ap, fmt);
	kvformat(tty_print, fmt, ap, NULL);
	va_end(ap);
	tty_lock_release();
}

/*
 * klog_printf - formatted output to the serial log (no timestamp).
 */
static void klog_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	kvformat(klog_writestr, fmt, ap, NULL);
	va_end(ap);
}

/*
 * klog - timestamped entry to the serial log (includes current psid).
 * Guarded by klog_lock to prevent interleaved output from concurrent tasks.
 * klog_printf() is safe to call under klog_lock because it does not acquire it.
 */
void klog(char *fmt, ...)
{
	va_list ap;
	task_struct *cur = CURRENT_TASK();

	mutex_lock(&klog_lock);
	klog_printf("[%d]: ", cur->psid);
	va_start(ap, fmt);
	kvformat(klog_writestr, fmt, ap, NULL);
	va_end(ap);

	mutex_unlock(&klog_lock);
}
