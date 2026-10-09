#include <device/time_internal.h>
#include <int/int.h>
#include <ps/ps.h>
#include <ps/smp.h>
#include <lib/lock.h>
#include <macro.h>

static int kvm_clock;
static unsigned long long clock_origin_us;
static unsigned long long clock_start_us;
static unsigned long long last_us;
static unsigned long long coarse_us;
static long long wall_offset_us;
static spinlock_t time_lock = SPINLOCK_INITIALIZER;

/* time_lock protects shared clock state and PIT ports across CPUs. Its IRQ
 * masking prevents local preemption and torn i386 64-bit accesses. Clock
 * reads never acquire a scheduler or network lock; release before callbacks. */
static unsigned long long monotonic_us(void)
{
	unsigned long long now;
	if (kvm_clock) {
		now = time_kvm_read_us();
		now = (now >= clock_origin_us ? now - clock_origin_us : 0) +
		      clock_start_us;
	} else {
		now = time_pit_read_us();
	}
	if (now < last_us)
		now = last_us;
	last_us = now;
	return now;
}

void time_init(void)
{
	int irq;
	spinlock_lock(&time_lock, &irq);
	wall_offset_us = (long long)time_rtc_read() * 1000000LL;
	time_pit_init();
	spinlock_unlock(&time_lock, irq);
}

void time_cpu_init(void)
{
	/* CPU setup enables SSE2 before this ordered TSC clock is selected.
	 * Preserve the PIT boot origin when selecting the faster source. */
	if (smp_cpu_id() == 0) {
		int irq;
		spinlock_lock(&time_lock, &irq);
		clock_start_us = monotonic_us();
		if (time_kvm_init()) {
			clock_origin_us = time_kvm_read_us();
			kvm_clock = 1;
		}
		spinlock_unlock(&time_lock, irq);
	} else if (kvm_clock) {
		time_kvm_cpu_init();
	}
}

const char *time_clock_name(void)
{
	return kvm_clock ? "kvm-clock" : "pit";
}

void time_tick(void)
{
	int irq;
	spinlock_lock(&time_lock, &irq);
	time_pit_tick();
	coarse_us = monotonic_us();
	spinlock_unlock(&time_lock, irq);
}

unsigned long long time_now_us(void)
{
	int irq;
	spinlock_lock(&time_lock, &irq);
	unsigned long long now = monotonic_us();
	spinlock_unlock(&time_lock, irq);
	return now;
}

unsigned long long time_now_ms(void)
{
	return time_now_us() / 1000;
}

unsigned long long time_coarse_ms(void)
{
	int irq;
	spinlock_lock(&time_lock, &irq);
	unsigned long long now = coarse_us;
	spinlock_unlock(&time_lock, irq);
	return now / 1000;
}

unsigned long long time_deadline_ms(unsigned long long delay_ms)
{
	unsigned long long us = time_now_us();
	unsigned long long now = us / 1000 + !!(us % 1000);
	return delay_ms > ~0ULL - now ? ~0ULL : now + delay_ms;
}

unsigned long long time_now_tickets(void)
{
	int irq;
	spinlock_lock(&time_lock, &irq);
	unsigned long long now = time_pit_ticks();
	spinlock_unlock(&time_lock, irq);
	return now;
}

unsigned long long time_wall_us(void)
{
	int irq;
	spinlock_lock(&time_lock, &irq);
	unsigned long long now = (long long)monotonic_us() + wall_offset_us;
	spinlock_unlock(&time_lock, irq);
	return now;
}

unsigned long time_wall_sec(void)
{
	int irq;
	spinlock_lock(&time_lock, &irq);
	unsigned long long now = (long long)coarse_us + wall_offset_us;
	spinlock_unlock(&time_lock, irq);
	return (unsigned long)(now / 1000000ULL);
}

void time_set_wall_offset(long long wall_us)
{
	int irq;
	spinlock_lock(&time_lock, &irq);
	wall_offset_us = wall_us - (long long)monotonic_us();
	spinlock_unlock(&time_lock, irq);
}

void time_sync_rtc(void)
{
	unsigned long epoch = time_rtc_read();
	int irq;
	spinlock_lock(&time_lock, &irq);
	wall_offset_us =
		(long long)epoch * 1000000LL - (long long)monotonic_us();
	spinlock_unlock(&time_lock, irq);
}

void ms_to_timeval(unsigned ms, struct timeval *tv)
{
	tv->tv_sec = ms / 1000;
	tv->tv_usec = (ms % 1000) * 1000;
}

void us_to_timeval(unsigned long long us, struct timeval *tv)
{
	tv->tv_sec = us / 1000000ULL;
	tv->tv_usec = us % 1000000ULL;
}

void msleep(unsigned int ms)
{
	if (!ms)
		return;
	if (ms < TICK_MS)
		delay(ms * 1000);
	else
		time_wait(ms);
}

void usleep(unsigned int us)
{
	if (us < TICK_MS * 1000)
		delay(us);
	else
		msleep(us / 1000 + !!(us % 1000));
}
