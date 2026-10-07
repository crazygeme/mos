#include <ps/usage.h>
#include <ps/smp.h>
#include <int/interrupt.h>
#include <lib/klib.h>

/* Keep independently written CPU counters on separate cache lines. */
static struct {
	cpu_ticks_t ticks;
} __attribute__((aligned(64))) cpu_usage[SMP_MAX_CPUS];

void ps_usage_init(task_struct *task, task_struct *parent, int share)
{
	if (share) {
		task->usage = parent->usage;
		__sync_add_and_fetch(&task->usage->refs, 1);
	} else {
		task->usage = zalloc(sizeof(*task->usage));
		task->usage->refs = 1;
	}
}

void ps_usage_put(task_struct *task)
{
	if (task->usage && __sync_sub_and_fetch(&task->usage->refs, 1) == 0)
		kfree(task->usage);
	task->usage = NULL;
}

/* A timer sample charges only the task executing on its receiving CPU.
 * The timer runs with local interrupts disabled; scheduler ownership prevents
 * another CPU from charging this task until it migrates. Callers supplying
 * synthetic tasks/counters must likewise keep their local counters private.
 */
void ps_usage_charge(task_struct *task, cpu_ticks_t *cpu, int user)
{
	if (!task->stats ||
	    (task->type == ps_kernel && task->priority == ps_idle)) {
		ps_usage_add_local(&cpu->idle, 1);
		return;
	}
	if (user) {
		ps_usage_add_local(&task->stats->user_tickets, 1);
		__atomic_fetch_add(&task->usage->user_tickets, 1,
				   __ATOMIC_RELAXED);
		ps_usage_add_local(&cpu->user, 1);
	} else {
		ps_usage_add_local(&task->stats->kernel_tickets, 1);
		__atomic_fetch_add(&task->usage->kernel_tickets, 1,
				   __ATOMIC_RELAXED);
		ps_usage_add_local(&cpu->system, 1);
	}
}

void ps_account_tick(intr_frame *frame)
{
	if (ps_enabled())
		ps_usage_charge(current, &cpu_usage[smp_cpu_id()].ticks,
				arch_interrupt_frame_is_user(frame));
}

void ps_cpu_usage(unsigned cpu, cpu_usage_t *usage)
{
	usage->user = ps_usage_read(&cpu_usage[cpu].ticks.user);
	usage->system = ps_usage_read(&cpu_usage[cpu].ticks.system);
	usage->idle = ps_usage_read(&cpu_usage[cpu].ticks.idle);
}
