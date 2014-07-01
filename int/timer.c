#include <int/timer.h>
#include <int/int.h>
#include <ps/ps.h>
#include <int/dsr.h>

static unsigned long tickets;
static unsigned long seconds;
static unsigned long minutes;
static unsigned long hourse;
static unsigned long days;
static unsigned long total_seconds;

static unsigned long cycle_per_ticket;

static unsigned long rtc_get_time (void);

/* Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

static void timer_dsr(void* param);
static void timer_process(intr_frame* frame)
{
	// printf("timer process\n");
    tickets++;

    if ( tickets  == HZ) {
        seconds++;
        total_seconds++;
        tickets = 0;
    }

    if (seconds == 60) {
        minutes ++;
        seconds = 0;
    }

    if (minutes == 60) {
        hourse ++;
        minutes = 0;
    }

    if (hourse == 24) {
        days ++;
        hourse = 0;
    }

    dsr_add(timer_dsr, 0);


}



static void __attribute__ ((noinline))
busy_wait (unsigned int loops) 
{
  while (loops-- > 0)
    barrier ();
}


static int
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  unsigned int start = tickets;
  while (tickets == start)
    barrier ();

  /* Run LOOPS loops. */
  start = tickets;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != tickets;
}


static void timer_calibrate (void) 
{
  unsigned high_bit, test_bit;


  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  cycle_per_ticket = 1u << 10;
  while (!too_many_loops (cycle_per_ticket << 1)) 
    {
      cycle_per_ticket <<= 1;
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = cycle_per_ticket;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      cycle_per_ticket |= test_bit;

}

void timer_calculate_cpu_cycle()
{

    return timer_calibrate();

}

static void timer_dsr(void* param)
{
    task_struct* cur = CURRENT_TASK();

    if (!ps_enabled()) {
        return;
    }

    cur->remain_ticks--; 
    if (cur->remain_ticks <= 0) {
        cur->remain_ticks = DEFAULT_TASK_TIME_SLICE;
        if ((!cur->is_switching)) {
            task_sched(); 
        }
    }
}

void timer_init()
{
    timer_control control;

    tickets = 0;
    seconds = 0;
    minutes = 0;
    hourse = 0;
    days = 0;
    cycle_per_ticket = 0;
    total_seconds = 0;

	int_register(0x20, timer_process, 0, 0);

    control.channel = CHANNEL_0;
    control.bcd_mode = BCD_16_DIGIT_BINARY;
    control.access_mode = ACCESS_MODE_BOTH;
    control.operating_mode = OPERATION_MODE_RATE;

    _write_port(TIMER_CONTROL_MASK, *((unsigned char *)&control));
    _write_port(TIMER_CHANNEL_0, LATCH & 0xff);
    _write_port(TIMER_CHANNEL_0, LATCH  >> 8);


}

void timer_current(time_t* time)
{
    time->seconds = total_seconds;
    time->milliseconds = tickets*10;
}

void msleep(unsigned int ms)
{
    time_t base;
    timer_current(&base);

	if (ms < (1000/HZ)){
		usleep( ms * 1000 );
		return;
	}

    do {
        time_t now;
        unsigned long long total_milli = 0;
        timer_current(&now);
        total_milli = (unsigned long long)now.seconds * 1000 + (unsigned long long)now.milliseconds
             - (unsigned long long)base.seconds*1000 - (unsigned long long)base.milliseconds;
        if (total_milli < ms) {
            task_sched();
        }else{
            break;
        }
    }while (1);
}

void usleep(unsigned int us)
{
	

	if (us >= ((1000/HZ)*1000) )
	  return msleep( us / 1000);

    delay(us);

}

void delay(unsigned int us)
{
    unsigned cycles = 0;
	cycles = (unsigned int) ( ((double)cycle_per_ticket / (double)(1000 * 1000)) * HZ * us ); 
	// printk("usleep %d us equals %d cycles\n", us, cycles);
	busy_wait(cycles);
}

unsigned long time(unsigned long* t)
{
    unsigned long now = rtc_get_time();
    if (t) {
        *t = now;
    }
    return now;
}



/* This code is an interface to the MC146818A-compatible real
   time clock found on PC motherboards.  See [MC146818A] for
   hardware details. */

/* I/O register addresses. */
#define CMOS_REG_SET	0x70    /* Selects CMOS register exposed by REG_IO. */
#define CMOS_REG_IO	0x71    /* Contains the selected data byte. */

/* Indexes of CMOS registers with real-time clock functions.
   Note that all of these registers are in BCD format,
   so that 0x59 means 59, not 89. */
#define RTC_REG_SEC	0       /* Second: 0x00...0x59. */
#define RTC_REG_MIN	2       /* Minute: 0x00...0x59. */
#define RTC_REG_HOUR	4       /* Hour: 0x00...0x23. */
#define RTC_REG_MDAY	7	/* Day of the month: 0x01...0x31. */
#define RTC_REG_MON	8       /* Month: 0x01...0x12. */
#define RTC_REG_YEAR	9	/* Year: 0x00...0x99. */

/* Indexes of CMOS control registers. */
#define RTC_REG_A	0x0a    /* Register A: update-in-progress. */
#define RTC_REG_B	0x0b    /* Register B: 24/12 hour time, irq enables. */
#define RTC_REG_C	0x0c    /* Register C: pending interrupts. */
#define RTC_REG_D	0x0d    /* Register D: valid time? */

/* Register A. */
#define RTCSA_UIP	0x80	/* Set while time update in progress. */

/* Register B. */
#define	RTCSB_SET	0x80	/* Disables update to let time be set. */
#define RTCSB_DM	0x04	/* 0 = BCD time format, 1 = binary format. */
#define RTCSB_24HR	0x02    /* 0 = 12-hour format, 1 = 24-hour format. */

static int bcd_to_bin (unsigned char);
static unsigned char cmos_read (unsigned char index);

/* Returns number of seconds since Unix epoch of January 1,
   1970. */
static unsigned long rtc_get_time (void)
{
  static const int days_per_month[12] =
    {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
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
  do
    {
      sec = bcd_to_bin (cmos_read (RTC_REG_SEC));
      min = bcd_to_bin (cmos_read (RTC_REG_MIN));
      hour = bcd_to_bin (cmos_read (RTC_REG_HOUR));
      mday = bcd_to_bin (cmos_read (RTC_REG_MDAY));
      mon = bcd_to_bin (cmos_read (RTC_REG_MON));
      year = bcd_to_bin (cmos_read (RTC_REG_YEAR));
    }
  while (sec != bcd_to_bin (cmos_read (RTC_REG_SEC)));

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
static int
bcd_to_bin (unsigned char x)
{
  return (x & 0x0f) + ((x >> 4) * 10);
}

/* Reads a byte from the CMOS register with the given INDEX and
   returns the byte read. */
static unsigned char
cmos_read (unsigned char index)
{
  _write_port (CMOS_REG_SET, index);
  return _read_port (CMOS_REG_IO);
}


