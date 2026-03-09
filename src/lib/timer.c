#include <ps.h>
#include <time.h>
#include <timer.h>
#include <rbtree.h>
#include <lock.h>

typedef struct _timer_control {
	hash_table *timers;
	mutex_t lock;
	cond_t event;
} timer_control_t;

struct timer_t {
	int enabled;
	int repeatly;
	unsigned period;
	void *context;
	timeout_t callback;
};

static timer_control_t control;

static int timer_comp(void *k1, void *k2)
{
	int l = (int)k1;
	int r = (int)k2;
	if (l == r)
		return 1;

	return l - r;
}

unsigned timer_wakeup_times = 0;
unsigned timer_process_times = 0;

void do_timer_loop()
{
	/*
	 * Timer worker loop:
	 * - Wait until at least one timer is present.
	 * - Take the earliest timer (hash_first).
	 * - Sleep until its due time, then invoke the callback if still enabled.
	 * - Remove the timer; if periodic and enabled, reschedule it.
	 *
	 * Concurrency notes:
	 * - We take the timer snapshot (key/value) under the lock, then
	 *   release the lock before sleeping and invoking the callback.
	 * - Only this loop removes timers; external code only inserts and
	 *   can disable a timer. We recheck 'enabled' before calling.
	 *
	 * Scheduling note:
	 * - Periodic timers are rescheduled using current time + period.
	 *   This avoids backlog accumulation if callback/sleep overruns.
	 */
	for (;;) {
		mutex_lock(&control.lock);
		if (hash_isempty(control.timers)) {
			mutex_unlock(&control.lock);
			/* Sleep until a new timer is started */
			cond_wait(&control.event);
			timer_wakeup_times++;
			continue;
		}

		key_value_pair *kv = hash_first(control.timers);
		if (!kv) {
			/* Defensive: queue became empty, clear pending event */
			cond_reset(&control.event);
			mutex_unlock(&control.lock);
			continue;
		}

		unsigned due_at = (unsigned)kv->key;
		timer_t *t = (timer_t *)kv->val;
		mutex_unlock(&control.lock);

		timer_wakeup_times++;

		/* Wait until the timer expires */
		unsigned now = time_now_ms();
		if (now < due_at)
			msleep(due_at - now);

		/* Invoke callback if timer is still enabled */
		if (t->enabled && t->callback)
			t->callback(t, t->context);

		/* Remove and optionally reschedule */
		mutex_lock(&control.lock);
		hash_remove_at(control.timers, kv);
		if (t->enabled && t->repeatly) {
			unsigned next_due = time_now_ms() + t->period;
			hash_insert(control.timers, next_due, t);
		} else {
			free(t);
		}

		/* If no timers remain, clear the event flag */
		if (hash_isempty(control.timers))
			cond_reset(&control.event);

		mutex_unlock(&control.lock);

		timer_process_times++;
	}
}

void timer_init()
{
	control.timers = hash_create(timer_comp);
	mutex_init(&control.lock);
	cond_init(&control.event, "timer", 0);
}

timer_t *timer_start(timeout_t callback, unsigned period, int repeatly,
		     void *ctx)
{
	timer_t *entry = malloc(sizeof(*entry));
	int empty = 1;
	unsigned timeout = time_now_ms() + period;

	entry->callback = callback;
	entry->period = period;
	entry->repeatly = repeatly;
	entry->context = ctx;
	entry->enabled = 1;

	mutex_lock(&control.lock);
	empty = hash_isempty(control.timers);
	hash_insert(control.timers, timeout, entry);
	mutex_unlock(&control.lock);

	if (empty)
		cond_notify(&control.event);

	return entry;
}

void timer_stop(timer_t *timer)
{
	timer->repeatly = 0;
	timer->enabled = 0;
}
