#include <device/time.h>
#include <device/time_internal.h>
#include <int/int.h>
#include <ps/ps.h>
#include <ps/smp.h>
#include <syscall/syscall.h>
#include <macro.h>
#include <test/test.h>

KTEST(Timekeeping, GregorianRTC)
{
	const struct time_calendar dates[] = { { 0, 0, 0, 1, 1, 1970 },
					       { 0, 0, 0, 29, 2, 2000 },
					       { 0, 0, 0, 1, 3, 2024 },
					       { 0, 0, 0, 1, 1, 2026 },
					       { 0, 0, 0, 1, 3, 2026 } };
	const unsigned long expected[] = { 0, 951782400UL, 1709251200UL,
					   1767225600UL, 1772323200UL };
	for (unsigned i = 0; i < sizeof(dates) / sizeof(dates[0]); i++)
		EXPECT_EQ(time_rtc_epoch(&dates[i]), expected[i]);
	return 0;
}

KTEST(Timekeeping, ClockDomains)
{
	unsigned long long mono = time_now_us();
	unsigned long long wall = time_wall_us();
	EXPECT_LT(mono, 86400000000ULL);
	EXPECT_GT(wall, 1000000000000000ULL);
	EXPECT_LE(time_coarse_ms(), time_now_ms());
	klog("Clock source: %s\n", time_clock_name());
	return 0;
}

KTEST(Timekeeping, WallChangesPreserveMonotonic)
{
	unsigned long long start = time_now_us(), saved = time_wall_us();
	time_set_wall_offset((long long)saved + 3600000000LL);
	unsigned long long forward = time_now_us();
	time_set_wall_offset((long long)saved - 3600000000LL);
	unsigned long long backward = time_now_us();
	time_set_wall_offset((long long)saved + (time_now_us() - start));
	EXPECT_GE(forward, start);
	EXPECT_GE(backward, forward);
	EXPECT_LT(backward - start, 1000000ULL);
	return 0;
}

KTEST(Timekeeping, RepeatedReadsAndScheduling)
{
	unsigned long long previous = time_now_us();
	for (unsigned i = 0; i < 10000; i++) {
		unsigned long long now = time_now_us();
		ASSERT_GE(now, previous);
		previous = now;
		if (!(i % 1000))
			task_sched();
	}
	return 0;
}

KTEST(Timekeeping, DelayedIRQ)
{
	int irq = int_intr_disable();
	unsigned long long before = time_now_us();
	/* Cover a PIT reload and multiple reads before its IRQ is serviced. */
	delay(15000);
	unsigned long long first = time_now_us();
	unsigned long long second = time_now_us();
	int_intr_setlevel(irq);
	EXPECT_GE(first, before);
	EXPECT_GE(second, first);
	EXPECT_GE(time_now_us(), second);
	return 0;
}

struct clock_readers {
	unsigned parent, arrivals, completed, errors, cpus;
};

static void parallel_clock_reader(void *param)
{
	struct clock_readers *state = param;
	current->life->ppid = state->parent;
	current->life->exit_signal = 0;
	sched_disable();
	unsigned long long deadline = time_deadline_ms(2000);
	__sync_or_and_fetch(&state->cpus, 1U << smp_cpu_id());
	__atomic_add_fetch(&state->arrivals, 1, __ATOMIC_RELEASE);
	while (__atomic_load_n(&state->arrivals, __ATOMIC_ACQUIRE) != 2 &&
	       time_now_ms() < deadline)
		PAUSE();
	if (__atomic_load_n(&state->arrivals, __ATOMIC_ACQUIRE) != 2)
		__sync_add_and_fetch(&state->errors, 1);
	unsigned long long previous = time_now_us();
	unsigned long long ticks = time_now_tickets();
	for (unsigned i = 0; i < 10000; i++) {
		unsigned long long coarse = time_coarse_ms();
		unsigned long long now = time_now_us();
		unsigned long long next_ticks = time_now_tickets();
		if (now < previous || coarse > now / 1000 || next_ticks < ticks)
			__sync_add_and_fetch(&state->errors, 1);
		previous = now;
		ticks = next_ticks;
		if (!(i % 1000)) {
			struct time_calendar calendar;
			time_rtc_calendar(&calendar);
			if (calendar.sec > 59 || calendar.min > 59 ||
			    calendar.hour > 23 || calendar.mday < 1 ||
			    calendar.mday > 31 || calendar.mon < 1 ||
			    calendar.mon > 12 || calendar.year < 1970)
				__sync_add_and_fetch(&state->errors, 1);
			struct mos_sigevent event = { .notify = 1 };
			struct mos_itimerspec setting = { .it_value = { 1,
									0 } };
			struct mos_itimerspec remaining;
			int id;
			if (sys_timer_create((i / 1000) & 1, &event, &id)) {
				__sync_add_and_fetch(&state->errors, 1);
			} else {
				if (sys_timer_settime(id, 0, &setting, NULL) ||
				    sys_timer_gettime(id, &remaining))
					__sync_add_and_fetch(&state->errors, 1);
				if (sys_timer_delete(id))
					__sync_add_and_fetch(&state->errors, 1);
			}
		}
	}
	sched_enable();
	__atomic_add_fetch(&state->completed, 1, __ATOMIC_RELEASE);
	do_exit(0);
}

KTEST(Timekeeping, ParallelClocksAndTimers)
{
	if (smp_cpu_count() < 2)
		return 0;
	struct clock_readers *state = zalloc(sizeof(*state));
	ASSERT_NONNULL(state);
	state->parent = current->life->psid;
	unsigned pids[2], created = 0;
	for (unsigned i = 0; i < 2; i++) {
		unsigned pid = ps_create(parallel_clock_reader, state,
					 ps_normal, ps_kernel);
		if ((int)pid < 0)
			break;
		pids[created++] = pid;
		current->life->nchildren++;
	}
	while (__atomic_load_n(&state->completed, __ATOMIC_ACQUIRE) < created)
		time_wait(1);
	for (unsigned i = 0; i < created; i++) {
		int status = -1;
		EXPECT_EQ(do_waitpid(pids[i], &status, 0, NULL), (int)pids[i]);
		EXPECT_EQ(status, 0);
	}
	EXPECT_EQ(created, 2U);
	EXPECT_EQ(state->errors, 0U);
	EXPECT_NE(state->cpus & (state->cpus - 1), 0U);
	kfree(state);
	return 0;
}
