#include <hw/time.h>
#include <int/int.h>
#include <ps/ps.h>
#include <lib/port.h>
#include <config.h>
#include <macro.h>

extern void do_signal(intr_frame *frame);

static unsigned long tickets;
static unsigned long total_tickets;
static unsigned long seconds;
static unsigned long minutes;
static unsigned long hourse;
static unsigned long days;
static unsigned long total_seconds;
static unsigned long cycle_per_ticket;
static unsigned long rtc_get_time(void);
static int is_force_switching = 0;

static void force_switch(short ds);
static void time_process(intr_frame *frame)
{
	tickets++;
	total_tickets++;

	if (tickets == HZ) {
		seconds++;
		total_seconds++;
		tickets = 0;
	}

	if (seconds == 60) {
		minutes++;
		seconds = 0;
	}

	if (minutes == 60) {
		hourse++;
		minutes = 0;
	}

	if (hourse == 24) {
		days++;
		hourse = 0;
	}

	BARRIER();

	if (!sched_is_enabled())
		return;

	if (__sync_add_and_fetch(&(is_force_switching), 0) == 0) {
		__sync_add_and_fetch(&(is_force_switching), 1);
		force_switch(frame->ds);
	}

	/* Deliver pending signals when returning to user space.
	 * This ensures alarm() and kill() work even in tight user-space loops. */
	do_signal(frame);
}

static void __attribute__((noinline)) busy_wait(unsigned int loops)
{
	while (loops-- > 0)
		BARRIER();
}

static int too_many_loops(unsigned loops)
{
	/* Wait for a time tick. */
	unsigned int start = tickets;
	while (tickets == start)
		BARRIER();

	/* Run LOOPS loops. */
	start = tickets;
	busy_wait(loops);

	/* If the tick count changed, we iterated too long. */
	BARRIER();
	return start != tickets;
}

static void time_calibrate(void)
{
	unsigned high_bit, test_bit;

	/* Approximate loops_per_tick as the largest power-of-two
       still less than one time tick. */
	cycle_per_ticket = 1u << 10;
	while (!too_many_loops(cycle_per_ticket << 1)) {
		cycle_per_ticket <<= 1;
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = cycle_per_ticket;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10;
	     test_bit >>= 1)
		if (!too_many_loops(high_bit | test_bit))
			cycle_per_ticket |= test_bit;
}

void time_calculate_cpu_cycle()
{
	return time_calibrate();
}

/* Return CPU speed in MHz based on the calibrated loops-per-tick value. */
unsigned time_get_cpu_mhz(void)
{
	/* cycle_per_ticket loops per tick, HZ ticks/second:
	 *   MHz = cycle_per_ticket * HZ / 1_000_000 = cycle_per_ticket / 10_000 */
	return (unsigned)(cycle_per_ticket / 10000);
}

static void force_switch(short ds)
{
	task_struct *cur = CURRENT_TASK();

	if (!ps_enabled() || !sched_is_enabled()) {
		__sync_add_and_fetch(&(is_force_switching), -1);
		return;
	}

	cur->remain_ticks--;
	if (ds == KERNEL_DATA_SELECTOR)
		cur->kernel_tickets++;
	else
		cur->user_tickets++;

	if (cur->remain_ticks <= 0) {
		cur->remain_ticks = DEFAULT_TASK_TIME_SLICE;
		cur->niv_switches++;
		task_sched();
	}
	__sync_add_and_fetch(&(is_force_switching), -1);
}

void time_init()
{
	time_control control;

	tickets = 0;
	total_tickets = 0;
	seconds = 0;
	minutes = 0;
	hourse = 0;
	days = 0;
	cycle_per_ticket = 0;
	total_seconds = 0;

	int_register(0x20, time_process, 0, 0);

	control.channel = CHANNEL_0;
	control.bcd_mode = BCD_16_DIGIT_BINARY;
	control.access_mode = ACCESS_MODE_BOTH;
	control.operating_mode = OPERATION_MODE_RATE;

	port_write_byte(TIME_CONTROL_MASK, *((unsigned char *)&control));
	port_write_byte(TIME_CHANNEL_0, LATCH & 0xff);
	port_write_byte(TIME_CHANNEL_0, LATCH >> 8);
}

void time_current(time_t *t)
{
	time_t now = time(0);
	t->seconds = now.time / 1000;
	t->milliseconds = now.time - t->seconds * 1000;
}

unsigned time_now_ms()
{
	BARRIER();
	return total_tickets * (1000 / HZ);
}

void ms_to_timeval(unsigned ms, struct timeval *tv)
{
	tv->tv_sec = (long)(ms / 1000);
	tv->tv_usec = (long)(ms - (unsigned long long)tv->tv_sec * 1000) * 1000;
}

void us_to_timeval(unsigned long long us, struct timeval *tv)
{
	tv->tv_sec = (int)(us / 1000000ULL);
	tv->tv_usec = (int)(us - (unsigned long long)tv->tv_sec * 1000000ULL);
}

/* Read the current PIT channel-0 countdown value (LATCH..0). */
static unsigned pit_read_count(void)
{
	port_write_byte(TIME_CONTROL_MASK, 0x00); /* latch channel 0 */
	unsigned lo = port_read_byte(TIME_CHANNEL_0);
	unsigned hi = port_read_byte(TIME_CHANNEL_0);
	return (hi << 8) | lo;
}

unsigned long long time_now_us()
{
#define TICK_US (1000000ULL / HZ) /* microseconds per PIT tick (10000) */
	unsigned long t1, t2;
	unsigned count;

	/* Re-read if a tick fires between sampling total_tickets and the PIT. */
	do {
		BARRIER();
		t1 = total_tickets;
		count = pit_read_count();
		BARRIER();
		t2 = total_tickets;
	} while (t1 != t2);

	/* PIT counts DOWN from LATCH to 0; convert remaining count to elapsed us.
	 * Resolution: 1 / CLOCK_TICK_RATE ≈ 0.84 μs, no calibration needed. */
	unsigned elapsed = (count <= LATCH) ? (LATCH - count) : 0;
	unsigned long long frac =
		(unsigned long long)elapsed * 1000000ULL / CLOCK_TICK_RATE;

	return (unsigned long long)t1 * TICK_US + frac;
#undef TICK_US
}

unsigned long long time_now_precisely()
{
	unsigned low, high;
	unsigned long long cycle;
	__asm("pushl %eax");
	__asm("pushl %edx");
	__asm("rdtsc");
	__asm("movl %%eax, %0" : "=r"(low));
	__asm("movl %%edx, %0" : "=r"(high));
	__asm("popl %eax");
	__asm("popl %edx");
	cycle = ((unsigned long long)high) << 32 | low;
	return cycle;
}

unsigned long long cycle_to_us(unsigned long long dur_cycles)
{
	unsigned long long cycle_per_micro_second;
	unsigned long long tick;
	cycle_per_micro_second = (unsigned long long)cycle_per_ticket / 10000;
	if (cycle_per_micro_second)
		tick = dur_cycles / cycle_per_micro_second;
	else
		tick = 0;
	return tick;
}

unsigned long long cycle_to_ms(unsigned long long dur_cycles)
{
	return cycle_to_us(dur_cycles) / 1000;
}

void msleep(unsigned int ms)
{
	task_struct *cur = CURRENT_TASK();
	unsigned timeout = time_now_ms() + ms;
	unsigned now = 0;

	if (ms == 0) {
		return;
	}

	if (ms < (1000 / HZ)) {
		usleep(ms * 1000);
		return;
	}

	do {
		now = time_now_ms();
		if (now >= timeout)
			break;

		cur->timeout = timeout;
		task_sched();
		cur->timeout = 0;

	} while (1);
}

void usleep(unsigned int us)
{
	if (us >= ((1000 / HZ) * 1000))
		return msleep(us / 1000);

	delay(us);
}

void delay(unsigned int us)
{
	unsigned cycles = 0;
	cycles = (unsigned int)(((double)cycle_per_ticket /
				 (double)(1000 * 1000)) *
				HZ * us);
	// printk("usleep %d us equals %d cycles\n", us, cycles);
	busy_wait(cycles);
}

time_t time(time_t *t)
{
	unsigned long long now = time_now_ms();
	time_t ret;
	ret.time = now;
	if (t) {
		t->time = now;
	}
	return ret;
}

/* This code is an interface to the MC146818A-compatible real
   time clock found on PC motherboards.  See [MC146818A] for
   hardware details. */

/* I/O register addresses. */
#define CMOS_REG_SET 0x70 /* Selects CMOS register exposed by REG_IO. */
#define CMOS_REG_IO 0x71 /* Contains the selected data byte. */

/* Indexes of CMOS registers with real-time clock functions.
   Note that all of these registers are in BCD format,
   so that 0x59 means 59, not 89. */
#define RTC_REG_SEC 0 /* Second: 0x00...0x59. */
#define RTC_REG_MIN 2 /* Minute: 0x00...0x59. */
#define RTC_REG_HOUR 4 /* Hour: 0x00...0x23. */
#define RTC_REG_MDAY 7 /* Day of the month: 0x01...0x31. */
#define RTC_REG_MON 8 /* Month: 0x01...0x12. */
#define RTC_REG_YEAR 9 /* Year: 0x00...0x99. */

/* Indexes of CMOS control registers. */
#define RTC_REG_A 0x0a /* Register A: update-in-progress. */
#define RTC_REG_B 0x0b /* Register B: 24/12 hour time, irq enables. */
#define RTC_REG_C 0x0c /* Register C: pending interrupts. */
#define RTC_REG_D 0x0d /* Register D: valid time? */

/* Register A. */
#define RTCSA_UIP 0x80 /* Set while time update in progress. */

/* Register B. */
#define RTCSB_SET 0x80 /* Disables update to let time be set. */
#define RTCSB_DM 0x04 /* 0 = BCD time format, 1 = binary format. */
#define RTCSB_24HR 0x02 /* 0 = 12-hour format, 1 = 24-hour format. */

static int bcd_to_bin(unsigned char);
static unsigned char cmos_read(unsigned char index);

/* Returns number of seconds since Unix epoch of January 1,
   1970. */
static unsigned long rtc_get_time(void)
{
	static const int days_per_month[12] = { 31, 28, 31, 30, 31, 30,
						31, 31, 30, 31, 30, 31 };
	int sec, min, hour, mday, mon, year;
	unsigned long time;
	int i;

	/* Get time components.

       We repeatedly read the time until it is stable from one read
       to another, in case we start our initial read in the middle
       of an update.  This strategy is not recommended by the
       MC146818A datasheet, but it is simpler than any of their
       suggestions and, furthermore, it is also used by Linux.

       The MC146818A can be configured for BCD or binary format,
       but for historical reasons everyone always uses BCD format
       except on obscure non-PC platforms, so we don't bother
       trying to detect the format in use. */
	do {
		sec = bcd_to_bin(cmos_read(RTC_REG_SEC));
		min = bcd_to_bin(cmos_read(RTC_REG_MIN));
		hour = bcd_to_bin(cmos_read(RTC_REG_HOUR));
		mday = bcd_to_bin(cmos_read(RTC_REG_MDAY));
		mon = bcd_to_bin(cmos_read(RTC_REG_MON));
		year = bcd_to_bin(cmos_read(RTC_REG_YEAR));
	} while (sec != bcd_to_bin(cmos_read(RTC_REG_SEC)));

	/* Translate years-since-1900 into years-since-1970.
       If it's before the epoch, assume that it has passed 2000.
       This will break at 2070, but that's long after our 31-bit
       time_t breaks in 2038. */
	if (year < 70)
		year += 100;
	year -= 70;

	/* Break down all components into seconds. */
	time = (year * 365 + (year - 1) / 4) * 24 * 60 * 60;
	for (i = 1; i <= mon; i++)
		time += days_per_month[i - 1] * 24 * 60 * 60;
	if (mon > 2 && year % 4 == 0)
		time += 24 * 60 * 60;
	time += (mday - 1) * 24 * 60 * 60;
	time += hour * 60 * 60;
	time += min * 60;
	time += sec;

	return time;
}

/* Returns the integer value of the given BCD byte. */
static int bcd_to_bin(unsigned char x)
{
	return (x & 0x0f) + ((x >> 4) * 10);
}

/* Reads a byte from the CMOS register with the given INDEX and
   returns the byte read. */
static unsigned char cmos_read(unsigned char index)
{
	port_write_byte(CMOS_REG_SET, index);
	return port_read_byte(CMOS_REG_IO);
}
