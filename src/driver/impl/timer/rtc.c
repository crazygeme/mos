#include <device/time_internal.h>
#include <int/int.h>
#include <lib/port.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <macro.h>

#define RTC_INDEX 0x70
#define RTC_DATA 0x71
#define RTC_UIP 0x80
#define RTC_BINARY 0x04
#define RTC_24H 0x02

/* CMOS index/data ports are shared by all CPUs, including /dev/rtc reads. */
static spinlock_t rtc_lock = { .inited = 1 };

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

unsigned long time_rtc_read(void)
{
	struct time_calendar calendar;
	time_rtc_calendar(&calendar);
	return time_rtc_epoch(&calendar);
}
