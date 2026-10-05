#ifndef MOS_TIME_INTERNAL_H
#define MOS_TIME_INTERNAL_H

#include <device/time.h>

struct time_calendar {
	unsigned sec, min, hour, mday, mon, year;
};
void time_rtc_calendar(struct time_calendar *calendar);
unsigned long time_rtc_epoch(const struct time_calendar *calendar);

void time_pit_init(void);
void time_pit_tick(void);
unsigned long long time_pit_read_us(void);
unsigned long long time_pit_ticks(void);
unsigned long time_rtc_read(void);
void time_tick(void);
int time_kvm_init(void);
void time_kvm_cpu_init(void);
unsigned long long time_kvm_read_us(void);

#endif
