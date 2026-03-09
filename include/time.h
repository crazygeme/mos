#ifndef _TIME_H_
#define _TIME_H_

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

typedef struct _time_t {
	union {
		struct {
			unsigned long seconds;
			unsigned long milliseconds;
		};
		unsigned long long time;
	};

} time_t;

struct timeval {
	int tv_sec; /* seconds */
	int tv_usec; /* microseconds */
};

struct timezone {
	int tz_minuteswest; /* minutes west of Greenwich */
	int tz_dsttime; /* type of DST correction */
};

struct timespec {
	int tv_sec; /* seconds */
	int tv_nsec; /* nanoseconds */
};

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

#define HZ 100 /* 100 interrupts per second */
#define CLOCK_TICK_RATE 1193180
#define LATCH ((CLOCK_TICK_RATE + HZ / 2) / HZ)

void time_init();

void time_calculate_cpu_cycle();

unsigned time_get_cpu_mhz(void);

void time_current(time_t *time);

unsigned time_now_ms();

unsigned long long time_now_us();

unsigned long long time_now_precisely();

unsigned long long cycle_to_ms(unsigned long long dur_cycles);

unsigned long long cycle_to_us(unsigned long long dur_cycles);

void ms_to_timeval(unsigned ms, struct timeval *tv);

void msleep(unsigned int ms);

void usleep(unsigned int us);

void delay(unsigned int us);

time_t time(time_t *t);

void enable_scheduler();
void disable_scheduler();

#endif
