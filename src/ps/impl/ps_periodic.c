/*
 * ps_periodic.c — low-frequency kernel background services.
 *
 * Owns:
 *   - a shared periodic kernel task for process-context housekeeping
 *   - graphics-VT refresh pacing
 */

#include <ps/ps.h>
#include <device/chardev.h>
#include <lib/klib.h>

#define GRAPHICS_REFRESH_FPS 60
#define GRAPHICS_REFRESH_MS (1000 / GRAPHICS_REFRESH_FPS)

static void ps_system_service_task(void *param)
{
	unsigned long long next_timer_ms;
	unsigned long long next_graphics_ms;

	(void)param;

	next_timer_ms = time_now_ms() + TICK_MS;
	next_graphics_ms = time_now_ms() + GRAPHICS_REFRESH_MS;

	for (;;) {
		unsigned long long now = time_now_ms();
		unsigned long long next_due;
		unsigned sleep_ms;

		if (now >= next_timer_ms) {
			ps_timer_poll();

			next_timer_ms +=
				((now - next_timer_ms) / TICK_MS + 1) * TICK_MS;
		}

		if (chardev_console_needs_refresh() && now >= next_graphics_ms) {
			chardev_console_refresh();
			next_graphics_ms += ((now - next_graphics_ms) /
						     GRAPHICS_REFRESH_MS +
					     1) *
					    GRAPHICS_REFRESH_MS;
		}

		next_due = next_timer_ms;
		if (chardev_console_needs_refresh() && next_graphics_ms < next_due)
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
