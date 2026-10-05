/* Alarm expiration is independent of the recipient's syscall/wait path. */
#include <ps/ps.h>
#include <device/time.h>
#include "ps_internal.h"

/* Protected by ps_lock; only armed alarms are linked here. */
static struct rb_root alarms;

void ps_alarm_disarm_unsafe(task_struct *task)
{
	if (!RB_EMPTY_NODE(&task->alarm_rb)) {
		rb_erase(&task->alarm_rb, &alarms);
		RB_CLEAR_NODE(&task->alarm_rb);
	}
	task->alarm_expire_ms = 0;
}

static void alarm_arm_unsafe(task_struct *task)
{
	struct rb_node **link = &alarms.rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		task_struct *other = rb_entry(*link, task_struct, alarm_rb);
		parent = *link;
		if (task->alarm_expire_ms < other->alarm_expire_ms)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&task->alarm_rb, parent, link);
	rb_insert_color(&task->alarm_rb, &alarms);
}

/* Process context only. Arguments are kernel locals, never user pointers.
 * Use the existing monotonic clock so changing wall time cannot move alarms.
 */
void ps_alarm_update(task_struct *task, int set, unsigned long long *value,
		     unsigned long long *interval)
{
	unsigned long long now, remaining, old_interval;
	int irq;

	spinlock_lock(&ps_lock, &irq);
	now = time_now_ms();
	remaining = task->alarm_expire_ms > now ? task->alarm_expire_ms - now :
						  0;
	old_interval = task->alarm_interval_ms;
	if (set) {
		ps_alarm_disarm_unsafe(task);
		task->alarm_interval_ms = *interval;
		if (*value) {
			task->alarm_expire_ms = time_deadline_ms(*value);
			alarm_arm_unsafe(task);
		}
	}
	*value = remaining;
	*interval = old_interval;
	spinlock_unlock(&ps_lock, irq);
}

/* IRQ0 has published a monotonic clock sample.
 * Expiration uses that sample without accessing clock hardware again and is
 * independent of service-task scheduling.
 */
void ps_alarm_tick(void)
{
	unsigned long long now;
	struct rb_node *node;
	int irq, wake = 0;

	if (RB_EMPTY_ROOT(&alarms))
		return;
	spinlock_lock(&ps_lock, &irq);
	now = time_coarse_ms();
	while ((node = rb_first(&alarms))) {
		task_struct *task = rb_entry(node, task_struct, alarm_rb);
		unsigned long long due = task->alarm_expire_ms;
		int waiting;

		if (due > now)
			break;
		ps_alarm_disarm_unsafe(task);
		if (task->status == ps_dying || !task->signal)
			continue;
		if (task->alarm_interval_ms) {
			/* Skip missed periods without accumulating phase drift. */
			unsigned long long interval = task->alarm_interval_ms;
			task->alarm_expire_ms =
				due + ((now - due) / interval + 1) * interval;
			alarm_arm_unsafe(task);
		}
		waiting = task->status == ps_waiting;
		ps_queue_signal_unsafe(task, SIGALRM);
		wake |= waiting && task->status == ps_ready;
	}
	if (wake)
		current->remain_ticks = 0;
	spinlock_unlock(&ps_lock, irq);
}
