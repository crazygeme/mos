#include <driver/driver.h>
#include <fs/entries.h>
#include <lib/klib.h>
#include <mm/mm.h>
#include <ps/smp.h>
#include <macro.h>
#include <int/int.h>
#include <lib/port.h>
#include <lib/lock.h>
#include <driver/driver.h>
#include <ps/ps.h>
#include <config.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <device/devnode.h>
#include <errno.h>
#include <lib/command.h>
#include <unistd.h>
#include <device/devnums.h>

static void rtc_dev_register(void);

#define TIME_CHANNEL_0 0x40 //       Channel 0 data port (read/write)
#define TIME_CHANNEL_1 0x41 //       Channel 1 data port (read/write) , unusable
#define TIME_CHANNEL_2 \
	0x42 //       Channel 2 data port (read/write) , this is a speaker whatever..
#define TIME_CONTROL_MASK \
	0x43 //       Mode/Command register (write only, a read is ignored)

__attribute__((aligned(1))) typedef struct _time_control {
	unsigned bcd_mode : 1;
	unsigned operating_mode : 3;
	unsigned access_mode : 2;
	unsigned channel : 2;
} time_control;

#define CHANNEL_0 0 //= Channel 0
#define CHANNEL_1 1 //= Channel 1
#define CHANNEL_2 2 //= Channel 2
#define CHANNEL_READ_BACK 3 // Read-back command (8254 only)

#define ACCESS_MODE_LATCH 0 //                 0 0 = Latch count value command
#define ACCESS_MODE_LOBYTE 1 //                 0 1 = Access mode: lobyte only
#define ACCESS_MODE_HIBYTE 2 //                 1 0 = Access mode: hibyte only
#define ACCESS_MODE_BOTH 3 //                 1 1 = Access mode: lobyte/hibyte

#define OPERATION_MODE_INTR 0 //(interrupt on terminal count)
#define OPERATION_MODE_ONESHOT 1 //(hardware re-triggerable one-shot)
#define OPERATION_MODE_RATE 2 //(rate generator)
#define OPERATION_MODE_WAVE 3 //(square wave generator)
#define OPERATION_MODE_SOFTSTROBE 4 //(software triggered strobe)
#define OPERATION_MODE_HWSTROBE 5 // (hardware triggered strobe)
#define OPERATION_MODE_RATE_ 6 //(rate generator, same as 010b)
#define OPERATION_MODE_WAVE_ 7 //(square wave generator, same as 011b)

#define BCD_16_DIGIT_BINARY 0
#define BCD_FOUR_DIGIT_BCD 1

#define CLOCK_TICK_RATE 1193180
#define LATCH ((CLOCK_TICK_RATE + HZ / 2) / HZ)
static void time_pit_init(void);
static void time_pit_tick(void);
static unsigned long long time_pit_read_us(void);
static unsigned long long time_pit_ticks(void);
static unsigned long time_rtc_read(void);
static void time_tick(void);
static int time_kvm_init(void);
static void time_kvm_cpu_init(void);
static unsigned long long time_kvm_read_us(void);

/* KVM's versioned 32-byte ABI. Each vCPU owns a permanently mapped slot. */
struct kvm_time_info {
	volatile unsigned version;
	unsigned pad0;
	volatile unsigned long long tsc_timestamp;
	volatile unsigned long long system_time;
	volatile unsigned multiplier;
	volatile signed char shift;
	volatile unsigned char flags;
	unsigned char pad[2];
} __attribute__((packed));

static struct kvm_time_info clocks[SMP_MAX_CPUS] __attribute__((aligned(64)));

static unsigned long long read_tsc(void)
{
	unsigned low, high;
	/* SSE2 is required below. LFENCE orders the preceding ABI field loads. */
	asm volatile("lfence; rdtsc; lfence"
		     : "=a"(low), "=d"(high)
		     :
		     : "memory");
	return ((unsigned long long)high << 32) | low;
}

static void time_kvm_cpu_init(void)
{
	paddr_t physical = VIRT_TO_PHY(&clocks[smp_cpu_id()]);
	arch_cpu_write_msr(0x4b564d01, (unsigned)physical | 1,
			   (unsigned)((unsigned long long)physical >> 32));
}

static int time_kvm_init(void)
{
	unsigned a, b, c, d;
	arch_cpu_cpuid(1, 0, &a, &b, &c, &d);
	if (!(c & (1U << 31)) || !(d & (1U << 4)) || !(d & (1U << 5)) ||
	    !(d & (1U << 26)))
		return 0;
	arch_cpu_cpuid(0x40000000, 0, &a, &b, &c, &d);
	if (b != 0x4b4d564b || c != 0x564b4d56 || d != 0x4d ||
	    (a && a < 0x40000001))
		return 0;
	arch_cpu_cpuid(0x40000001, 0, &a, &b, &c, &d);
	if (!(a & (1U << 3)))
		return 0;
	time_kvm_cpu_init();
	return 1;
}

static unsigned long long time_kvm_read_us(void)
{
	struct kvm_time_info *clock = &clocks[smp_cpu_id()];
	unsigned version, multiplier;
	int shift;
	unsigned long long timestamp, base, delta;
	for (;;) {
		version = clock->version;
		if (version & 1)
			continue;
		BARRIER();
		timestamp = clock->tsc_timestamp;
		base = clock->system_time;
		multiplier = clock->multiplier;
		shift = clock->shift;
		delta = read_tsc() - timestamp;
		BARRIER();
		if (version == clock->version)
			break;
	}
	if (shift < 0)
		delta >>= -shift;
	else
		delta <<= shift;
	/* Preserve the high 32 bits of a 64-by-32 multiply on both architectures. */
	delta = (delta >> 32) * multiplier +
		(((delta & 0xffffffffULL) * multiplier) >> 32);
	return (base + delta) / 1000;
}

#define RTC_INDEX 0x70
#define RTC_DATA 0x71
#define RTC_UIP 0x80
#define RTC_BINARY 0x04
#define RTC_24H 0x02

/* CMOS index/data ports are shared by all CPUs, including /dev/rtc reads. */
static spinlock_t rtc_lock = SPINLOCK_INITIALIZER;

static unsigned char rtc_register(unsigned reg)
{
	port_write_byte(RTC_INDEX, reg);
	return port_read_byte(RTC_DATA);
}

static void rtc_snapshot(unsigned char fields[7])
{
	while (rtc_register(0x0a) & RTC_UIP)
		BARRIER();
	fields[0] = rtc_register(0);
	fields[1] = rtc_register(2);
	fields[2] = rtc_register(4);
	fields[3] = rtc_register(7);
	fields[4] = rtc_register(8);
	fields[5] = rtc_register(9);
	fields[6] = rtc_register(0x0b);
}

void time_rtc_calendar(struct time_calendar *calendar)
{
	unsigned char first[7], second[7];
	int irq;
	spinlock_lock(&rtc_lock, &irq);
	do {
		rtc_snapshot(first);
		rtc_snapshot(second);
	} while (memcmp(first, second, sizeof(first)) ||
		 (rtc_register(0x0a) & RTC_UIP));
	int pm = second[2] & 0x80;
	second[2] &= 0x7f;
	if (!(second[6] & RTC_BINARY)) {
		for (unsigned i = 0; i < 6; i++)
			second[i] = (second[i] & 15) + (second[i] >> 4) * 10;
	}
	if (!(second[6] & RTC_24H))
		second[2] = second[2] % 12 + (pm ? 12 : 0);
	calendar->sec = second[0];
	calendar->min = second[1];
	calendar->hour = second[2];
	calendar->mday = second[3];
	calendar->mon = second[4];
	calendar->year = second[5] + (second[5] < 70 ? 2000 : 1900);
	spinlock_unlock(&rtc_lock, irq);
}

static int leap_year(unsigned year)
{
	return !(year % 4) && ((year % 100) || !(year % 400));
}

unsigned long time_rtc_epoch(const struct time_calendar *calendar)
{
	static const unsigned days_in_month[] = { 31, 28, 31, 30, 31, 30,
						  31, 31, 30, 31, 30, 31 };
	unsigned days = 0;
	for (unsigned year = 1970; year < calendar->year; year++)
		days += 365 + leap_year(year);
	for (unsigned month = 1; month < calendar->mon; month++)
		days += days_in_month[month - 1] +
			(month == 2 && leap_year(calendar->year));
	days += calendar->mday - 1;
	return ((days * 24UL + calendar->hour) * 60 + calendar->min) * 60 +
	       calendar->sec;
}

static unsigned long time_rtc_read(void)
{
	struct time_calendar calendar;
	time_rtc_calendar(&calendar);
	return time_rtc_epoch(&calendar);
}

static volatile unsigned long long tickets;
static unsigned long cycle_per_ticket;
static unsigned long long sample_ticks;
static unsigned sample_count = LATCH;
static int pending_wrap;

static void time_process(intr_frame *frame)
{
	time_tick();
	smp_tick();
	if (ps_enabled())
		current->sched->remain_ticks--;
}

static void __attribute__((noinline)) busy_wait(unsigned int loops)
{
	while (loops-- > 0)
		BARRIER();
}

static int too_many_loops(unsigned loops)
{
	/* Wait for a time tick. */
	unsigned long long start = time_now_tickets();
	while (time_now_tickets() == start)
		BARRIER();

	/* Run LOOPS loops. */
	start = time_now_tickets();
	busy_wait(loops);

	/* If the tick count changed, we iterated too long. */
	BARRIER();
	return start != time_now_tickets();
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

static void time_pit_init(void)
{
	time_control control;

	tickets = 0;
	cycle_per_ticket = 0;

	int_register(0x20, time_process, 0, 0);

	control.channel = CHANNEL_0;
	control.bcd_mode = BCD_16_DIGIT_BINARY;
	control.access_mode = ACCESS_MODE_BOTH;
	control.operating_mode = OPERATION_MODE_RATE;

	port_write_byte(TIME_CONTROL_MASK, *((unsigned char *)&control));
	port_write_byte(TIME_CHANNEL_0, LATCH & 0xff);
	port_write_byte(TIME_CHANNEL_0, LATCH >> 8);
}

/* The time core's IRQ-masked lock serializes ticks and all PIT port reads.
 * A proven reload is remembered until IRQ0 advances the raw tick epoch. */
static unsigned long long time_pit_read_us(void)
{
	unsigned long long epoch = tickets;
	unsigned count;

	port_write_byte(TIME_CONTROL_MASK, 0x00);
	count = port_read_byte(TIME_CHANNEL_0);
	count |= (unsigned)port_read_byte(TIME_CHANNEL_0) << 8;
	if (epoch != sample_ticks)
		pending_wrap = 0;
	else if (count > sample_count) {
		port_write_byte(0x20, 0x0a);
		if (port_read_byte(0x20) & 1)
			pending_wrap = 1;
	}
	sample_ticks = epoch;
	sample_count = count;
	if (pending_wrap)
		epoch++;
	unsigned elapsed = count <= LATCH ? LATCH - count : 0;
	unsigned long long counts = epoch * LATCH + elapsed;
	/* Use the actual programmed divisor and avoid overflow after months of
	 * uptime by scaling only the remainder into microseconds. */
	return (counts / CLOCK_TICK_RATE) * 1000000ULL +
	       (counts % CLOCK_TICK_RATE) * 1000000ULL / CLOCK_TICK_RATE;
}

static unsigned long long time_pit_ticks(void)
{
	return tickets;
}

static void time_pit_tick(void)
{
	tickets++;
}

unsigned long long cycle_to_us(unsigned long long loops)
{
	return cycle_per_ticket ? loops * (1000000ULL / HZ) / cycle_per_ticket :
				  0;
}

unsigned long long cycle_to_ms(unsigned long long loops)
{
	return cycle_to_us(loops) / 1000;
}

void delay(unsigned int us)
{
	/* Chunking bounds the loop count and multiplication for long delays. */
	while (us) {
		unsigned chunk = us > 1000 ? 1000 : us;
		unsigned loops =
			(unsigned)(((unsigned long long)cycle_per_ticket * HZ *
					    chunk +
				    999999) /
				   1000000);
		busy_wait(loops);
		us -= chunk;
	}
}

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

static void time_tick(void)
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
/*
 * src/dev/rtc.c — /dev/rtc real-time clock device.
 *
 * Exposes the MC146818A-compatible CMOS RTC via:
 *   ioctl(fd, RTC_RD_TIME, &rtc_time)  — read current wall-clock time
 *
 * Character device, major 10, minor 135 (Linux-compatible).
 */

/* ── Linux-compatible RTC ioctls ─────────────────────────────────────────── */

#define RTC_RD_TIME 0x80247009 /* read time */
#define RTC_UIE_ON 0x7003 /* update interrupt enable on  */
#define RTC_UIE_OFF 0x7004 /* update interrupt enable off */

/* ── struct rtc_time — matches Linux uapi ────────────────────────────────── */

struct rtc_time {
	int tm_sec; /* 0-59 */
	int tm_min; /* 0-59 */
	int tm_hour; /* 0-23 */
	int tm_mday; /* 1-31 */
	int tm_mon; /* 0-11  (Linux convention) */
	int tm_year; /* years since 1900 */
	int tm_wday; /* 0-6, Sunday = 0 */
	int tm_yday; /* 0-365 */
	int tm_isdst;
};

/* Share the hardware snapshot and calendar conversion with boot timekeeping. */
static void rtc_read_time(struct rtc_time *t)
{
	struct time_calendar calendar, jan1;
	time_rtc_calendar(&calendar);
	t->tm_sec = calendar.sec;
	t->tm_min = calendar.min;
	t->tm_hour = calendar.hour;
	t->tm_mday = calendar.mday;
	t->tm_mon = calendar.mon - 1;
	t->tm_year = calendar.year - 1900;
	unsigned long epoch = time_rtc_epoch(&calendar);
	jan1 = calendar;
	jan1.mon = jan1.mday = 1;
	jan1.hour = jan1.min = jan1.sec = 0;
	t->tm_wday = (epoch / 86400 + 4) % 7;
	t->tm_yday = (epoch - time_rtc_epoch(&jan1)) / 86400;
	t->tm_isdst = 0;
}

/* ── VFS file operations ─────────────────────────────────────────────────── */

/* Set to 1 while UIE is active; rtc_read delivers one synthetic tick. */
static int rtc_uie_enabled;

static ssize_t rtc_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	/*
	 * When UIE is on, hwclock reads an unsigned long interrupt-count word
	 * to synchronise to the next 1 Hz boundary.  Return a count of 1 so
	 * the read completes immediately rather than blocking indefinitely.
	 */
	if (rtc_uie_enabled && size >= sizeof(unsigned long)) {
		unsigned long cnt = 1;
		memcpy(buf, &cnt, sizeof(cnt));
		return (ssize_t)sizeof(cnt);
	}
	return 0;
}

static unsigned rtc_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return (events & FS_POLL_WRITE) ? FS_POLL_WRITE : 0;
}

static int rtc_ioctl_rtc_rd_time(void *context __attribute__((unused)),
				 unsigned cmd __attribute__((unused)),
				 void *buf __attribute__((unused)))
{
	struct rtc_time t;
	rtc_read_time(&t);
	memcpy(buf, &t, sizeof(t));
	return 0;
}

static int rtc_ioctl_rtc_uie_on(void *context __attribute__((unused)),
				unsigned cmd __attribute__((unused)),
				void *buf __attribute__((unused)))
{
	rtc_uie_enabled = 1;
	return 0;
}

static int rtc_ioctl_rtc_uie_off(void *context __attribute__((unused)),
				 unsigned cmd __attribute__((unused)),
				 void *buf __attribute__((unused)))
{
	rtc_uie_enabled = 0;
	return 0;
}

static const command_operation rtc_commands[256] = {
	[RTC_RD_TIME & 255] = { RTC_RD_TIME, rtc_ioctl_rtc_rd_time },
	[RTC_UIE_ON & 255] = { RTC_UIE_ON, rtc_ioctl_rtc_uie_on },
	[RTC_UIE_OFF & 255] = { RTC_UIE_OFF, rtc_ioctl_rtc_uie_off },
};

static const command_operation *const rtc_command_groups[256] = {
	[(RTC_RD_TIME >> 8) & 255] = rtc_commands,
};

static int rtc_ioctl(file *fp, unsigned cmd, void *buf)
{
	return command_dispatch(rtc_command_groups, fp, cmd, buf, -ENOSYS);
}

static int rtc_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;

	memset(s, 0, sizeof(*s));
	s->st_atime = s->st_mtime = s->st_ctime = time_wall_sec();
	s->st_mode = node->i_mode;
	s->st_dev = MKDEV(10, 0);
	s->st_rdev = MKDEV(10, 135);
	s->st_nlink = 1;
	return 0;
}

static int rtc_release(file *fp)
{
	kfree(fp->f_inode);
	kfree(fp);
	return 0;
}

static const file_operations rtc_fops = {
	.getattr = rtc_getattr,
	.read = rtc_read,
	.poll = rtc_poll,
	.ioctl = rtc_ioctl,
	.release = rtc_release,
};

/* ── cdev dispatch ───────────────────────────────────────────────────────── */

static file *rtc_cdev_open(super_block *dev_sb, unsigned rdev, int flag)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &rtc_fops;
	return fp;
}

/* ── Registration ────────────────────────────────────────────────────────── */

static void rtc_dev_register(void)
{
	vfs_entry_device(devfs_entries(), "/rtc", S_IFCHR | 0660,
			 MKDEV(RTC_MAJOR, RTC_MINOR), "misc", rtc_cdev_open);
}

static int cmos_rtc_probe(uint32_t address)
{
	if (address != RTC_INDEX)
		return -ENODEV;
	rtc_dev_register();
	return 0;
}

static driver_t cmos_rtc_driver = {
	.name = "cmos-rtc",
	.bus = DEVICE_BUS_PLATFORM,
	.platform_address = RTC_INDEX,
	.probe_platform = cmos_rtc_probe,
	.early = 1,
};
DRIVER_REGISTER(cmos_rtc_driver);
