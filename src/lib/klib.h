#ifndef _LIB_KLIB_H
#define _LIB_KLIB_H
#include <mm/mm.h>
#include <config.h>
#include <macro.h>

#define HZ 100
#define TICK_MS (1000 / HZ)
#define DEFAULT_TASK_TIME_SLICE (500 / TICK_MS)
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

void time_init();
void time_cpu_init(void);
const char *time_clock_name(void);

void time_calculate_cpu_cycle();

unsigned time_get_cpu_mhz(void);

/* Boot-relative monotonic time. Wall-clock changes never move deadlines. */
unsigned long long time_now_us(void);

unsigned long long time_now_ms(void);
/* Last IRQ0 sample; safe for timer expiration checks without device I/O. */
unsigned long long time_coarse_ms(void);
/* Round a relative deadline upward; saturate on overflow. */
unsigned long long time_deadline_ms(unsigned long long delay_ms);

unsigned long long time_now_tickets();

/* Calendar time for inode timestamps, sampled at tick resolution. */
unsigned long time_wall_sec(void);

void time_set_wall_offset(long long wall_us);
void time_sync_rtc(void);

unsigned long long time_wall_us(void);

unsigned long long cycle_to_ms(unsigned long long dur_cycles);

unsigned long long cycle_to_us(unsigned long long dur_cycles);

void ms_to_timeval(unsigned ms, struct timeval *tv);
void us_to_timeval(unsigned long long us, struct timeval *tv);

void msleep(unsigned int ms);

void usleep(unsigned int us);

void delay(unsigned int us);

struct time_calendar {
	unsigned sec, min, hour, mday, mon, year;
};
void time_rtc_calendar(struct time_calendar *calendar);
unsigned long time_rtc_epoch(const struct time_calendar *calendar);

/* ── Heap ─────────────────────────────────────────────────────────────────── */

#ifndef NULL
#define NULL (void *)0
#endif

#include <stdarg.h>

#define kmalloc(size) malloc(size)
#define kfree(p) free(p)
#define kzalloc(size) zalloc(size)

#define klib_srand srand
#define klib_rand rand

void *malloc(unsigned int size);
void free(void *buf);
void *zalloc(unsigned size);
extern unsigned int heap_quota; /* current live-allocation byte count */

/* ── Init ─────────────────────────────────────────────────────────────────── */

void klib_init(void);

/* ── Logging ──────────────────────────────────────────────────────────────── */

void klog(char *str, ...);
void klog_close(void);
void klog_backend_init(void);
void klog_backend_putc(unsigned char byte);
void klog_backend_flush(void);

/* ── Formatted output ─────────────────────────────────────────────────────── */

void printk(const char *str, ...);
int printk_console_ready(void);

void vprintf(const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
void printf(const char *str, ...);
int sprintf(char *buf, const char *fmt, ...);

/* ── String / memory ──────────────────────────────────────────────────────── */

void memcpy(void *dst, const void *src, unsigned len);
void memmove(void *dst, const void *src, unsigned len);
int memcmp(const void *src, const void *dst, unsigned len);
void memset(void *src, char val, int len);

unsigned strlen(const char *str);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, int len);
char *strstr(const char *src, const char *str);
char *strrev(char *src);
int strcmp(const char *str, const char *dst);
int strncmp(const char *s1, const char *s2, int n);
char *strcat(char *str, const char *msg);
char *strchr(const char *str, char c);
char *strrchr(const char *str, char c);
char *strdup(const char *str);
const char *strglob(const char *pat, const char *str);

/* ── Character / number ───────────────────────────────────────────────────── */

int isspace(const char c);
int isprint(int c);
int tolower(int c);
int toupper(int c);
int islower(int c);
int isupper(int c);
char *itoa(int num, int base, int sign);
char *lltoa(long long num, int base, int sign);
int atoi(const char *str);
void srand(unsigned _seed);
unsigned int rand(void);
void kernel_random_bytes(void *buffer, unsigned size);
void kernel_random_mix(uint64_t value);

/* ── Misc ─────────────────────────────────────────────────────────────────── */

typedef struct _TEST_CONTROL {
	int verbose; /* verbose level: 0=off, 1=trace, 2=info */
	int text; /* boot SysV init in text mode (runlevel 3) */
	int test;
} TEST_CONTROL;
extern TEST_CONTROL TestControl;

#define TEST_LOG_OFF 0
#define TEST_LOG_ALWAYS 0
#define TEST_LOG_TRACE 1
#define TEST_LOG_INFO 2

#define TEST_LOG(level) (UNLIKELY(TestControl.verbose >= (level)))

#endif
