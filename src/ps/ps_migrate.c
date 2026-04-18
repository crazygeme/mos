#include <ps/ps.h>
#include <hw/cpu.h>
#include <lib/list.h>
#include <config.h>

#include "ps_internal.h"

extern void ret_from_fork(void);

static list_entry *ps_migrate_ready_queue_head(unsigned cpu, int priority)
{
	return &control.cpu[cpu].ready_queue[priority];
}

static int ps_task_reassignable_unsafe(const task_struct *task)
{
	if (!task)
		return 0;
	if (ncpus <= 1)
		return 0;
	if (task->psid == 0xffffffff)
		return 0;
	if (task->priority == ps_idle)
		return 0;
	if (task->type != ps_user)
		return 0;
	if (task->fork_flag & FORK_FLAG_VFORK)
		return 0;
	/*
	 * Fresh fork/clone children must return through ret_from_fork on the
	 * parent's CPU once so CPU-local TLS/LDT state is restored safely.
	 */
	if (task->tss.eip == (unsigned long)ret_from_fork)
		return 0;
	return 1;
}

static int ps_task_migratable_unsafe(const task_struct *task)
{
	return task && task->status == ps_ready &&
	       ps_task_reassignable_unsafe(task);
}

static unsigned ps_cpu_ready_load_unsafe(unsigned cpu)
{
	unsigned load = 0;
	int prio;

	for (prio = 0; prio < PS_PRIORITY_MAX; prio++) {
		list_entry *head = ps_migrate_ready_queue_head(cpu, prio);
		list_entry *node = head->next;

		while (node != head) {
			task_struct *task = container_of(node, task_struct, ps_list);

			if (task->status == ps_ready && task->priority != ps_idle)
				load++;
			node = node->next;
		}
	}

	if (cpu < MAX_CPUS && cpus[cpu].idle && cpus[cpu].idle->status != ps_running)
		load++;

	return load;
}

static unsigned ps_cpu_ready_load_prio_unsafe(unsigned cpu, int priority)
{
	unsigned load = 0;
	list_entry *head = ps_migrate_ready_queue_head(cpu, priority);
	list_entry *node = head->next;

	while (node != head) {
		task_struct *task = container_of(node, task_struct, ps_list);

		if (ps_task_migratable_unsafe(task))
			load++;
		node = node->next;
	}

	return load;
}

unsigned ps_migrate_select_cpu_unsafe(task_struct *task, unsigned src_cpu)
{
	unsigned cur_cpu = ps_task_cpu(task);
	unsigned best_cpu = cur_cpu;
	unsigned cur_load;
	unsigned best_load;
	unsigned cpu;

	(void)src_cpu;

	if (!ps_task_reassignable_unsafe(task))
		return cur_cpu;

	cur_load = ps_cpu_ready_load_unsafe(cur_cpu);
	best_load = cur_load;

	for (cpu = 0; cpu < (unsigned)ncpus; cpu++) {
		unsigned load;

		if (!cpus[cpu].online)
			continue;

		load = ps_cpu_ready_load_unsafe(cpu);
		if (load < best_load) {
			best_cpu = cpu;
			best_load = load;
		}
	}

	return best_cpu;
}

task_struct *ps_migrate_steal_unsafe(unsigned dst_cpu)
{
	int prio;

	if (ncpus <= 1 || dst_cpu >= (unsigned)ncpus || !cpus[dst_cpu].online)
		return NULL;

	for (prio = PS_PRIORITY_MAX - 1; prio >= 0; prio--) {
		unsigned donor_cpu = (unsigned)-1;
		unsigned donor_load = 0;
		unsigned cpu;

		for (cpu = 0; cpu < (unsigned)ncpus; cpu++) {
			unsigned load;

			if (cpu == dst_cpu || !cpus[cpu].online)
				continue;

			load = ps_cpu_ready_load_prio_unsafe(cpu, prio);
			if (load > donor_load) {
				donor_cpu = cpu;
				donor_load = load;
			}
		}

		if (donor_cpu != (unsigned)-1 && donor_load > 0) {
			list_entry *head =
				ps_migrate_ready_queue_head(donor_cpu, prio);
			list_entry *node = head->prev;

			while (node != head) {
				task_struct *task =
					container_of(node, task_struct, ps_list);

				node = node->prev;
				if (!ps_task_migratable_unsafe(task))
					continue;

				list_remove_entry(&task->ps_list);
				task->affinity = dst_cpu;
				list_insert_tail(
					ps_migrate_ready_queue_head(dst_cpu, prio),
					&task->ps_list);
				return task;
			}
		}
	}

	return NULL;
}
