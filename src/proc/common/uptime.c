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

static void fill(proc_buf_t *pb)
{
	unsigned long long now_ms = time_now_ms();
	unsigned secs = (unsigned)(now_ms / 1000);
	unsigned csecs = (unsigned)((now_ms % 1000) / 10);

	proc_buf_printf(pb, "%u.%02u 0.00\n", secs, csecs);
}

DEFINE_PROC_FILE(uptime, fill);
