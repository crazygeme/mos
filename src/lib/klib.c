/*
 * src/lib/klib.c - Kernel library initialisation and 64-bit arithmetic helpers.
 *
 * Companion modules:
 *   kmalloc.c  - heap allocator (malloc/free/calloc)
 *   kprint.c   - formatted output (printf/printk/klog)
 *   kstring.c  - string/memory/character functions
 *   tty.c      - TTY output, cursor management, ANSI escape processing
 */

#include <macro.h>
#include <klib.h>
#include <tty.h>

/* Defined in kmalloc.c */
extern void kmalloc_init(void);

/* Defined in kprint.c */
extern void klog_init(void);

void klib_init(void)
{
	tty_init();
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
