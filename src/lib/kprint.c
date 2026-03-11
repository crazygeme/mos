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
 */

#include <ps/ps.h>
#include <lib/lock.h>
#include <lib/klib.h>
#ifdef __DEBUG__
#include <hw/serial.h>
#endif
#include <hw/tty.h>
#include <hw/time.h>

/* ── Forward declarations (implemented in tty.c) ────────────────────────── */

extern void tty_print(char *str, void *ctx);

/* ── Locks ───────────────────────────────────────────────────────────────── */

extern spinlock_t tty_lock;

#ifdef __DEBUG__
static int klog_inited = 0;
#endif
static mutex_t klog_lock;

/* ── klog backend ────────────────────────────────────────────────────────── */

#ifdef __DEBUG__

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
	else
		klog_printf("\\%x", (unsigned char)c);
}

static void klog_writestr(char *str, void *ctx)
{
	if (!klog_inited || !str || !*str)
		return;
	while (*str)
		klog_write(*str++, ctx);
}

void klog_close(void)
{
	if (klog_inited)
		serial_flush();
}

#else /* !__DEBUG__ */

void klog_init(void)
{
	mutex_init(&klog_lock);
}

static void klog_write(char c, void *ctx)
{
	(void)c;
	(void)ctx;
}

static void klog_writestr(char *str, void *ctx)
{
	(void)str;
	(void)ctx;
}

void klog_close(void)
{
}

#endif /* __DEBUG__ */

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

	while (*p) {
		char cur = *p++;

		if (cur != '%') {
			EMIT(cur);
			continue;
		}

		cur = *p++;
		switch (cur) {
		case '%':
			EMIT('%');
			break;
		case 'd': {
			int v = va_arg(ap, int);
			char *s = itoa(v, 10, 1);

			EMITS(s);
			free(s);
			break;
		}
		case 'u': {
			unsigned v = va_arg(ap, unsigned);
			char *s = itoa((int)v, 10, 0);

			EMITS(s);
			free(s);
			break;
		}
		case 'x':
		case 'p': {
			unsigned v = va_arg(ap, unsigned);
			char *s = itoa((int)v, 16, 0);

			EMITS(s);
			free(s);
			break;
		}
		case 's': {
			char *v = va_arg(ap, char *);

			if (v)
				EMITS(v);
			break;
		}
		case 'c': {
			char v = (char)va_arg(ap, int);

			EMIT(v);
			break;
		}
		case 'b': {
			unsigned char v = (unsigned char)va_arg(ap, unsigned);
			char *s = itoa(v, 16, 0);
			/* Skip "0x", zero-pad to 2 digits */
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

	FLUSH();

#undef FLUSH
#undef EMIT
#undef EMITS
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* vprintf/vsprintf are the standard variadic-list entrypoints; all va_list
 * callers use these so there is no duplication of the format loop. */

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
	time_t t;
	char *ms_str;
	int pad, i;

	time_current(&t);
	ms_str = itoa(t.milliseconds, 10, 0);
	pad = 3 - (int)strlen(ms_str);

	spinlock_lock(&tty_lock);
	printf("[%d.", t.seconds);
	for (i = 0; i < pad; i++)
		tty_print("0", NULL);
	tty_print(ms_str, NULL);
	tty_print("] ", NULL);
	free(ms_str);

	va_start(ap, fmt);
	kvformat(tty_print, fmt, ap, NULL);
	va_end(ap);
	spinlock_unlock(&tty_lock);
}

/*
 * klog_printf - formatted output to the serial log (no timestamp).
 */
void klog_printf(const char *fmt, ...)
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
	time_t t;
	char *ms_str;
	int pad, i;

	mutex_lock(&klog_lock);

	time_current(&t);
	ms_str = itoa(t.milliseconds, 10, 0);
	pad = 3 - (int)strlen(ms_str);
	klog_printf("[%d: %d.", cur->psid, t.seconds);
	for (i = 0; i < pad; i++)
		klog_writestr("0", NULL);
	klog_writestr(ms_str, NULL);
	klog_writestr("] ", NULL);
	free(ms_str);

	va_start(ap, fmt);
	kvformat(klog_writestr, fmt, ap, NULL);
	va_end(ap);

	mutex_unlock(&klog_lock);
}
