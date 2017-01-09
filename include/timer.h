#ifndef _TIMER_H_
#define _TIMER_H_



#define TIMER_CHANNEL_0     0x40  //       Channel 0 data port (read/write)
#define TIMER_CHANNEL_1     0x41  //       Channel 1 data port (read/write) , unusable
#define TIMER_CHANNEL_2     0x42  //       Channel 2 data port (read/write) , this is a speaker whatever..
#define TIMER_CONTROL_MASK  0x43  //       Mode/Command register (write only, a read is ignored)

__attribute__ ((aligned (1))) 
typedef struct _timer_control
{
    unsigned bcd_mode : 1;
    unsigned operating_mode : 3;
    unsigned access_mode : 2;
    unsigned channel : 2;
}timer_control;

typedef struct _time_t
{
    union{
        struct{
        
            unsigned long seconds;
            unsigned long milliseconds;
        };
        unsigned long long time;
    };

}time_t;

struct timeval {
    long    tv_sec;         /* seconds */
    long    tv_usec;        /* microseconds */
};

struct timespec {
    long    tv_sec;         /* seconds */
    long    tv_nsec;        /* nanoseconds */
};
 
#define CHANNEL_0  0 //= Channel 0
#define CHANNEL_1  1 //= Channel 1
#define CHANNEL_2  2 //= Channel 2
#define CHANNEL_READ_BACK  3 // Read-back command (8254 only)

#define ACCESS_MODE_LATCH  0//                 0 0 = Latch count value command
#define ACCESS_MODE_LOBYTE  1//                 0 1 = Access mode: lobyte only
#define ACCESS_MODE_HIBYTE  2//                 1 0 = Access mode: hibyte only
#define ACCESS_MODE_BOTH  3//                 1 1 = Access mode: lobyte/hibyte

#define OPERATION_MODE_INTR  0 //(interrupt on terminal count)
#define OPERATION_MODE_ONESHOT  1 //(hardware re-triggerable one-shot)
#define OPERATION_MODE_RATE  2 //(rate generator)
#define OPERATION_MODE_WAVE  3 //(square wave generator)
#define OPERATION_MODE_SOFTSTROBE  4 //(software triggered strobe)
#define OPERATION_MODE_HWSTROBE  5// (hardware triggered strobe)
#define OPERATION_MODE_RATE_  6 //(rate generator, same as 010b)
#define OPERATION_MODE_WAVE_  7 //(square wave generator, same as 011b)


#define BCD_16_DIGIT_BINARY 0
#define BCD_FOUR_DIGIT_BCD 1

#define HZ 100 /* 100 interrupts per second */
#define CLOCK_TICK_RATE    1193180    
#define LATCH  ((CLOCK_TICK_RATE + HZ/2) / HZ) 

void timer_init();

void timer_calculate_cpu_cycle();

void timer_current(time_t* time);

unsigned time_now();

unsigned long long time_now_percisely();

unsigned cycle_to_ms(unsigned dur_cycles);

void msleep(unsigned int ms);

void usleep(unsigned int us);

void delay(unsigned int us);

time_t time(time_t* t);

#endif
