/* ITIMER_REAL belongs to the group, independently of task sleep timers. */
#include <ps/ps.h>
#include <lib/klib.h>
#include "ps_internal.h"
static struct rb_root alarms;

static void disarm(task_thread *group)
{
	if (!RB_EMPTY_NODE(&group->alarm_rb)) {
		rb_erase(&group->alarm_rb, &alarms);
		RB_CLEAR_NODE(&group->alarm_rb);
	}
	group->alarm_expire_ms = 0;
}

void ps_alarm_release_thread_group(task_thread *group)
{
	LOCK_GUARD(&ps_lock);
	disarm(group);
}

static task_struct *recipient(task_thread *group, task_struct *except)
{
	for (struct rb_node *node = rb_first(&control.mgr_queue); node;
	     node = rb_next(node)) {
		task_struct *task =
			(rb_entry(node, task_schedule, mgr_rb)->task);
		if (task != except && task->thread == group &&
		    task->sched->status != ps_dying)
			return task;
	}
	return NULL;
}

void ps_alarm_disarm_unsafe(task_struct *task)
{
	if (task->thread && !recipient(task->thread, task))
		disarm(task->thread);
}

static void arm(task_thread *group)
{
	struct rb_node **link = &alarms.rb_node, *parent = NULL;
	while (*link) {
		task_thread *other = rb_entry(*link, task_thread, alarm_rb);
		parent = *link;
		link = group->alarm_expire_ms < other->alarm_expire_ms ?
			       &parent->rb_left :
			       &parent->rb_right;
	}
	rb_link_node(&group->alarm_rb, parent, link);
	rb_insert_color(&group->alarm_rb, &alarms);
}

void ps_alarm_update(task_struct *task, int set, unsigned long long *value,
		     unsigned long long *interval)
{
	int irq;
	spinlock_lock(&ps_lock, &irq);
	task_thread *group = task->thread;
	unsigned long long now = time_now_ms();
	unsigned long long remaining =
		group->alarm_expire_ms > now ? group->alarm_expire_ms - now : 0;
	unsigned long long old_interval = group->alarm_interval_ms;
	if (set) {
		disarm(group);
		group->alarm_interval_ms = *interval;
		if (*value) {
			group->alarm_expire_ms = time_deadline_ms(*value);
			arm(group);
		}
	}
	*value = remaining;
	*interval = old_interval;
	spinlock_unlock(&ps_lock, irq);
}

void ps_alarm_tick(void)
{
	int irq;
	spinlock_lock(&ps_lock, &irq);
	unsigned long long now = time_coarse_ms();
	struct rb_node *node;
	while ((node = rb_first(&alarms))) {
		task_thread *group = rb_entry(node, task_thread, alarm_rb);
		unsigned long long due = group->alarm_expire_ms;
		if (due > now)
			break;
		disarm(group);
		task_struct *task = recipient(group, NULL);
		if (!task || !task->sighand)
			continue;
		if (group->alarm_interval_ms) {
			unsigned long long interval = group->alarm_interval_ms;
			group->alarm_expire_ms =
				due + ((now - due) / interval + 1) * interval;
			arm(group);
		}
		ps_queue_group_signal_unsafe(task, SIGALRM);
	}
	spinlock_unlock(&ps_lock, irq);
}
