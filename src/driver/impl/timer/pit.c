#include <device/time_internal.h>
#include <dev/tty.h>
#include <int/int.h>
#include <ps/ps.h>
#include <lib/port.h>
#include <config.h>
#include <macro.h>
#include <ps/smp.h>

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

void time_pit_init(void)
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
unsigned long long time_pit_read_us(void)
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

unsigned long long time_pit_ticks(void)
{
	return tickets;
}

void time_pit_tick(void)
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
