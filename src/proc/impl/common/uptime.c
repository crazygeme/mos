/*
 * /proc/uptime — system uptime and idle time.
 *
 * Format (Linux-compatible):
 *   <uptime_secs>.<centisecs> <idle_secs>.<centisecs>
 *
 * Uptime follows the monotonic clock, including delayed timer interrupts.
 */
#include "common.h"
#include <device/time.h>
#include <ps/ps.h>
#include <ps/usage.h>
#include <ps/smp.h>

static void fill(proc_buf_t *pb)
{
	unsigned long long ms = time_now_ms();
	unsigned up_sec = (unsigned)(ms / 1000);
	unsigned up_cs = (unsigned)((ms % 1000) / 10);

	unsigned long long it = 0;
	for (unsigned cpu = 0; cpu < smp_cpu_count(); cpu++) {
		cpu_usage_t sample;
		ps_cpu_usage(cpu, &sample);
		it += sample.idle;
	}
	unsigned idle_sec = (unsigned)(it / HZ);
	unsigned idle_cs = (unsigned)(it % HZ);

	proc_buf_printf(pb, "%u.%02u %u.%02u\n", up_sec, up_cs, idle_sec,
			idle_cs);
}

DEFINE_PROC_FILE(uptime, fill);
