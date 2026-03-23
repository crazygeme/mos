/*
 * /proc/uptime — system uptime and idle time.
 *
 * Format (Linux-compatible):
 *   <uptime_secs>.<centisecs> <idle_secs>.<centisecs>
 *
 * Uptime is derived from time_now_ms() which counts milliseconds since boot.
 * Idle time is not separately tracked; reported as 0.00.
 */
#include "common.h"

static void fill(void *buf, size_t size)
{
	unsigned long long now_ms = time_now_ms();
	unsigned secs = (unsigned)(now_ms / 1000);
	unsigned csecs = (unsigned)((now_ms % 1000) / 10);

	memset(buf, 0, size);
	sprintf(buf, "%u.%02u 0.00\n", secs, csecs);
}

DEFINE_PROC_FILE(uptime, fill);
