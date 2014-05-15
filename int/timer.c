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


  printk("cycle_per_ticket %d\n", cycle_per_ticket);

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
        if ((!CURRENT_TASK()->is_switching)) {
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

