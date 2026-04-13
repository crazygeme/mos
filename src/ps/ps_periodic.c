/*
 * ps_periodic.c — low-frequency kernel background services.
 *
 * Owns:
 *   - a shared periodic kernel task for process-context housekeeping
 *   - lwIP NO_SYS timer polling
 *   - graphics-VT refresh pacing
 */

#include <ps/ps.h>
#include <hw/tty.h>
#include <hw/time.h>
#include <lwip/timeouts.h>
#include <lwip/netif.h>

#define GRAPHICS_REFRESH_FPS 60
#define GRAPHICS_REFRESH_MS (1000 / GRAPHICS_REFRESH_FPS)

static void ps_system_service_task(void *param)
{
	unsigned long long next_lwip_ms;
	unsigned long long next_graphics_ms;

	(void)param;

	next_lwip_ms = time_now_ms() + TICK_MS;
	next_graphics_ms = time_now_ms() + GRAPHICS_REFRESH_MS;

	for (;;) {
		unsigned long long now = time_now_ms();
		unsigned long long next_due = next_lwip_ms;
		unsigned sleep_ms;

		if (now >= next_lwip_ms) {
			sys_check_timeouts();
			netif_poll_all();
			do {
				next_lwip_ms += TICK_MS;
			} while (next_lwip_ms <= now);
		}

		if (now >= next_graphics_ms) {
			tty_refresh_graphics();
			do {
				next_graphics_ms += GRAPHICS_REFRESH_MS;
			} while (next_graphics_ms <= now);
		}

		if (next_graphics_ms < next_due)
			next_due = next_graphics_ms;

		now = time_now_ms();
		sleep_ms = next_due > now ? (unsigned)(next_due - now) : 1;
		if (sleep_ms == 0)
			sleep_ms = 1;
		time_wait(sleep_ms);
	}
}

void ps_start_system_services(void)
{
	static int started;

	if (started)
		return;
	started = 1;
	ps_create(ps_system_service_task, NULL, ps_normal, ps_kernel);
}
