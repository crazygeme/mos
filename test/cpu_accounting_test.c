#include <ps/usage.h>
#include <ps/smp.h>
#include <int/int.h>
#include <syscall/syscall.h>
#include <lib/klib.h>
#include <test/test.h>

KTEST(CPUAccounting, UserSystemAndIdle)
{
	task_stats_t stats = { 0 };
	task_thread group = { 0 };
	task_struct task = { .stats = &stats,
			     .thread = &group,
			     .sched = &(task_schedule){ 0 },
			     .life = &(task_lifecycle){ .type = ps_user } };
	cpu_ticks_t cpu = { 0 };
	ps_usage_charge(&task, &cpu, 1);
	ps_usage_charge(&task, &cpu, 0);
	EXPECT_EQ(task_utime(&task), 1ULL);
	EXPECT_EQ(ps_usage_read(&stats.kernel_tickets), 1ULL);
	EXPECT_EQ(ps_usage_read(&group.user_tickets), 1ULL);
	EXPECT_EQ(ps_usage_read(&group.kernel_tickets), 1ULL);
	EXPECT_EQ(cpu.user, 1ULL);
	EXPECT_EQ(cpu.system, 1ULL);
	task.life->type = ps_kernel;
	task.sched->priority = ps_idle;
	ps_usage_charge(&task, &cpu, 0);
	EXPECT_EQ(cpu.idle, 1ULL);
	EXPECT_EQ(ps_usage_read(&group.kernel_tickets), 1ULL);
	task.sched->priority = ps_normal;
	ps_usage_charge(&task, &cpu, 0);
	EXPECT_EQ(cpu.system, 2ULL);
	return 0;
}

KTEST(CPUAccounting, SleepingAndZombieCounters)
{
	task_stats_t stats = { .start_tickets = 1, .user_tickets = 19 };
	task_struct task = { .stats = &stats,
			     .sched =
				     &(task_schedule){ .status = ps_waiting } };
	EXPECT_EQ(task_utime(&task), 19ULL);
	task.sched->status = ps_dying;
	EXPECT_EQ(task_utime(&task), 19ULL);
	stats.user_tickets = 0xffffffffULL;
	task_thread group = { 0 };
	task.thread = &group;
	task.life = &(task_lifecycle){ .type = ps_user };
	cpu_ticks_t cpu = { 0 };
	ps_usage_charge(&task, &cpu, 1);
	EXPECT_EQ(task_utime(&task),
		  sizeof(ps_tick_t) == 4 ? 0ULL : 0x100000000ULL);
	return 0;
}

KTEST(CPUAccounting, NativeWidthRollover)
{
	EXPECT_EQ(sizeof(ps_tick_t), sizeof(uintptr_t));
	ps_tick_t max = (ps_tick_t)-1;
	task_stats_t stats = { .user_tickets = max, .kernel_tickets = max };
	task_thread group = { .user_tickets = max,
			      .kernel_tickets = max,
			      .child_utime = max };
	task_struct task = { .stats = &stats,
			     .thread = &group,
			     .sched = &(task_schedule){ 0 },
			     .life = &(task_lifecycle){ .type = ps_user } };
	cpu_ticks_t cpu = { .user = max, .system = max, .idle = max };
	ps_usage_charge(&task, &cpu, 1);
	ps_usage_charge(&task, &cpu, 0);
	EXPECT_EQ(task_utime(&task), 0ULL);
	EXPECT_EQ(ps_usage_read(&stats.kernel_tickets), 0ULL);
	EXPECT_EQ(ps_usage_read(&group.user_tickets), 0ULL);
	EXPECT_EQ(ps_usage_read(&group.kernel_tickets), 0ULL);
	EXPECT_EQ(cpu.user, 0UL);
	EXPECT_EQ(cpu.system, 0UL);
	task.life->type = ps_kernel;
	task.sched->priority = ps_idle;
	ps_usage_charge(&task, &cpu, 0);
	EXPECT_EQ(cpu.idle, 0UL);
	ps_usage_add_local(&group.child_utime, 2);
	EXPECT_EQ(ps_usage_read(&group.child_utime), 1ULL);
	/* Snapshots widen before conversions, even on i386. */
	group.child_stime = 0xffffffffUL;
	EXPECT_EQ(ps_usage_read(&group.child_stime) * (1000000ULL / HZ),
		  42949672950000ULL);
	return 0;
}

KTEST(CPUAccounting, ReportingAndCPUClockResolution)
{
	task_thread group = { .user_tickets = 123,
			      .kernel_tickets = 45,
			      .child_utime = 67,
			      .child_stime = 89 };
	task_struct *task = current;
	int irq = int_intr_disable();
	task_thread *saved = task->thread;
	task->thread = &group;
	struct tms times;
	rusage usage;
	EXPECT_GE(sys_times(&times), 0L);
	EXPECT_EQ(times.tms_utime, 123);
	EXPECT_EQ(times.tms_stime, 45);
	EXPECT_EQ(times.tms_cutime, 67);
	EXPECT_EQ(times.tms_cstime, 89);
	EXPECT_EQ(sys_getrusage(RUSAGE_SELF, &usage), 0);
	EXPECT_EQ(usage.ru_utime.tv_sec, 1);
	EXPECT_EQ(usage.ru_utime.tv_usec, 230000);
	EXPECT_EQ(usage.ru_stime.tv_usec, 450000);
	EXPECT_EQ(sys_getrusage(RUSAGE_CHILDREN, &usage), 0);
	EXPECT_EQ(usage.ru_utime.tv_usec, 670000);
	EXPECT_EQ(usage.ru_stime.tv_usec, 890000);
	struct timespec stamp;
	EXPECT_EQ(sys_clock_gettime(2, &stamp), 0);
	EXPECT_EQ(stamp.tv_sec, 1);
	EXPECT_EQ(stamp.tv_nsec, 680000000);
#if defined(__x86_64__)
	extern intptr_t native_times(void *);
	int64_t wire[4];
	group.user_tickets = 0x100000001ULL;
	EXPECT_GE(native_times(wire), (intptr_t)0);
	EXPECT_EQ(wire[0], (int64_t)0x100000001ULL);
#endif
	task->thread = saved;
	int_intr_setlevel(irq);
	/* Non-NULL resolution outputs require a userspace destination. */
	EXPECT_EQ(sys_clock_getres(2, NULL), 0);
	EXPECT_EQ(sys_clock_getres(3, NULL), 0);
	return 0;
}

struct concurrent_usage {
	task_thread group;
	unsigned parent, arrivals, completed, cpus;
};

static void charge_parallel(void *opaque)
{
	struct concurrent_usage *state = opaque;
	task_stats_t stats = { 0 };
	task_struct task = { .stats = &stats,
			     .thread = &state->group,
			     .sched = &(task_schedule){ 0 },
			     .life = &(task_lifecycle){ .type = ps_user } };
	cpu_ticks_t cpu = { 0 };
	current->life->ppid = state->parent;
	current->life->exit_signal = 0;
	sched_disable();
	unsigned long long deadline = time_deadline_ms(2000);
	__sync_or_and_fetch(&state->cpus, 1U << smp_cpu_id());
	__sync_add_and_fetch(&state->arrivals, 1);
	while (__sync_fetch_and_add(&state->arrivals, 0) != 2 &&
	       time_now_ms() < deadline)
		PAUSE();
	if (__sync_fetch_and_add(&state->arrivals, 0) == 2)
		for (unsigned i = 0; i < 25000; i++)
			ps_usage_charge(&task, &cpu, 1);
	sched_enable();
	__sync_add_and_fetch(&state->completed, 1);
}

KTEST(CPUAccounting, ConcurrentThreadGroupCounters)
{
	if (smp_cpu_count() < 2)
		return 0;
	/* Cross the native counter boundary while both CPUs update the group. */
	struct concurrent_usage state = {
		.group = { .user_tickets = (ps_tick_t)-25000 },
		.parent = current->life->psid
	};
	ps_create(charge_parallel, &state, ps_normal, ps_kernel);
	ps_create(charge_parallel, &state, ps_normal, ps_kernel);
	while (__sync_fetch_and_add(&state.completed, 0) != 2)
		time_wait(10);
	EXPECT_EQ(ps_usage_read(&state.group.user_tickets), 25000ULL);
	EXPECT_EQ(ps_usage_read(&state.group.kernel_tickets), 0ULL);
	EXPECT_NE(state.cpus & (state.cpus - 1), 0U);
	return 0;
}
