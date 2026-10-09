#include <ps/ps.h>
#include <device/time.h>
#include <errno.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <lib/lock.h>
#include <macro.h>
#include <syscall/syscall.h>

#define MOS_TIMER_COUNT 128
#define MOS_SIGEV_NONE 1
#define MOS_SIGEV_SIGNAL 0
#define MOS_SIGEV_THREAD_ID 4
#define MOS_TIMER_ABSTIME 1

struct mos_timer {
	unsigned owner;
	unsigned target;
	int id;
	int clockid;
	int wall_deadline;
	int notify;
	int signo;
	uintptr_t value;
	int overrun;
	unsigned long long due_ns;
	unsigned long long interval_ns;
	struct rb_node id_node;
	struct mos_timer *free_next;
};

static struct mos_timer timers[MOS_TIMER_COUNT];
static int next_timer_id = 1;
static struct rb_root timer_ids = _RBTREE_ROOT_INIT;
static struct mos_timer *free_timers;
static mutex_t timer_lock;

/* Timer syscalls, process exit and the service task may run on different CPUs.
 * A task-owned mutex also permits demand faults on syscall argument buffers. */
static void timers_init(void)
{
	mutex_init(&timer_lock);
	for (unsigned i = MOS_TIMER_COUNT; i; i--) {
		timers[i - 1].free_next = free_timers;
		free_timers = &timers[i - 1];
	}
}
KERNEL_INIT(7, timers_init);

static unsigned long long timer_now_ns(const struct mos_timer *timer)
{
	/* Relative timers measure elapsed time even when created on REALTIME.
	 * Only absolute REALTIME deadlines follow wall-clock adjustments. */
	return (timer->wall_deadline ? time_wall_us() : time_now_us()) *
	       1000ULL;
}

static int timer_timespec_ns(const struct timespec *value,
			     unsigned long long *result)
{
	if (value->tv_sec < 0 || value->tv_nsec < 0 ||
	    value->tv_nsec >= 1000000000)
		return -EINVAL;
	*result = (unsigned long long)value->tv_sec * 1000000000ULL +
		  (unsigned)value->tv_nsec;
	return 0;
}

static void timer_ns_timespec(unsigned long long value, struct timespec *result)
{
	result->tv_sec = (int)(value / 1000000000ULL);
	result->tv_nsec = (int)(value % 1000000000ULL);
}

static struct mos_timer *timer_lookup(int id)
{
	struct rb_node *node = timer_ids.rb_node;
	while (node) {
		struct mos_timer *timer =
			rb_entry(node, struct mos_timer, id_node);
		if (timer->id == id)
			return timer->owner == current->tgid ? timer : NULL;
		node = id < timer->id ? node->rb_left : node->rb_right;
	}
	return NULL;
}

static void timer_insert(struct mos_timer *timer)
{
	struct rb_node **link = &timer_ids.rb_node, *parent = NULL;
	while (*link) {
		struct mos_timer *other =
			rb_entry(*link, struct mos_timer, id_node);
		parent = *link;
		link = timer->id < other->id ? &parent->rb_left :
					       &parent->rb_right;
	}
	rb_init_node(&timer->id_node);
	rb_link_node(&timer->id_node, parent, link);
	rb_insert_color(&timer->id_node, &timer_ids);
}

static void timer_release(struct mos_timer *timer)
{
	rb_erase(&timer->id_node, &timer_ids);
	memset(timer, 0, sizeof(*timer));
	timer->free_next = free_timers;
	free_timers = timer;
}

int do_timer_create(int clockid, const struct mos_sigevent *event, int *timerid,
		    uintptr_t value)
{
	LOCK_GUARD(&timer_lock);
	struct mos_timer *timer;
	task_struct *target;
	int notify = event ? event->notify : MOS_SIGEV_SIGNAL;
	int signo = event ? event->signo : SIGALRM;

	if (!timerid)
		return -EFAULT;
	if (clockid != 0 && clockid != 1)
		return -EINVAL;
	if (notify != MOS_SIGEV_NONE && notify != MOS_SIGEV_SIGNAL &&
	    notify != (MOS_SIGEV_SIGNAL | MOS_SIGEV_THREAD_ID))
		return -EINVAL;
	if (notify != MOS_SIGEV_NONE && (signo < 1 || signo > SIGRTMIN_KERNEL))
		return -EINVAL;
	if (notify & MOS_SIGEV_THREAD_ID) {
		target = ps_find_process((unsigned)event->tid);
		if (!target || target->tgid != current->tgid)
			return -EINVAL;
	}
	timer = free_timers;
	if (!timer)
		return -EAGAIN;
	free_timers = timer->free_next;
	memset(timer, 0, sizeof(*timer));
	timer->owner = current->tgid;
	timer->target = event && (notify & MOS_SIGEV_THREAD_ID) ?
				(unsigned)event->tid :
				current->psid;
	timer->clockid = clockid;
	timer->notify = notify;
	timer->signo = signo;
	timer->value = value;
	timer->id = next_timer_id++;
	if (next_timer_id <= 0)
		next_timer_id = 1;
	timer_insert(timer);
	*timerid = timer->id;
	return 0;
}

int sys_timer_create(int clockid, const struct mos_sigevent *event,
		     int *timerid)
{
	return do_timer_create(clockid, event, timerid,
			       event ? (unsigned)event->value : 0);
}

int sys_timer_settime(int timerid, int flags,
		      const struct mos_itimerspec *value,
		      struct mos_itimerspec *old_value)
{
	LOCK_GUARD(&timer_lock);
	struct mos_timer *timer = timer_lookup(timerid);
	unsigned long long due, interval, now;
	int ret;

	if (!timer)
		return -EINVAL;
	if (!value)
		return -EFAULT;
	if (flags & ~MOS_TIMER_ABSTIME)
		return -EINVAL;
	ret = timer_timespec_ns(&value->it_value, &due);
	if (ret)
		return ret;
	ret = timer_timespec_ns(&value->it_interval, &interval);
	if (ret)
		return ret;
	now = timer_now_ns(timer);
	if (old_value) {
		timer_ns_timespec(timer->interval_ns, &old_value->it_interval);
		timer_ns_timespec(timer->due_ns > now ? timer->due_ns - now : 0,
				  &old_value->it_value);
	}
	timer->wall_deadline = timer->clockid == 0 &&
			       (flags & MOS_TIMER_ABSTIME);
	now = timer_now_ns(timer);
	timer->interval_ns = interval;
	timer->due_ns = due ? ((flags & MOS_TIMER_ABSTIME) ? due : now + due) :
			      0;
	timer->overrun = 0;
	return 0;
}

int sys_timer_gettime(int timerid, struct mos_itimerspec *value)
{
	LOCK_GUARD(&timer_lock);
	struct mos_timer *timer = timer_lookup(timerid);
	unsigned long long now;
	if (!timer)
		return -EINVAL;
	if (!value)
		return -EFAULT;
	now = timer_now_ns(timer);
	timer_ns_timespec(timer->interval_ns, &value->it_interval);
	timer_ns_timespec(timer->due_ns > now ? timer->due_ns - now : 0,
			  &value->it_value);
	return 0;
}

int sys_timer_getoverrun(int timerid)
{
	LOCK_GUARD(&timer_lock);
	struct mos_timer *timer = timer_lookup(timerid);
	return timer ? timer->overrun : -EINVAL;
}

int sys_timer_delete(int timerid)
{
	LOCK_GUARD(&timer_lock);
	struct mos_timer *timer = timer_lookup(timerid);
	if (!timer)
		return -EINVAL;
	timer_release(timer);
	return 0;
}

void ps_timer_discard_group(unsigned tgid)
{
	LOCK_GUARD(&timer_lock);
	struct rb_node *node, *next;
	for (node = rb_first(&timer_ids); node; node = next) {
		struct mos_timer *timer =
			rb_entry(node, struct mos_timer, id_node);
		next = rb_next(node);
		if (timer->owner == tgid)
			timer_release(timer);
	}
}

void ps_timer_poll(void)
{
	LOCK_GUARD(&timer_lock);
	struct rb_node *node;
	for (node = rb_first(&timer_ids); node; node = rb_next(node)) {
		struct mos_timer *timer =
			rb_entry(node, struct mos_timer, id_node);
		unsigned long long now;
		if (!timer->id || !timer->due_ns)
			continue;
		now = timer_now_ns(timer);
		if (now < timer->due_ns)
			continue;
		if (timer->interval_ns) {
			unsigned long long count =
				(now - timer->due_ns) / timer->interval_ns + 1;
			timer->due_ns += count * timer->interval_ns;
			timer->overrun = count > 1 ? (int)(count - 1) : 0;
		} else {
			timer->due_ns = 0;
		}
		if (timer->notify == MOS_SIGEV_NONE)
			continue;
		ps_timer_notify(timer->target, timer->signo, timer->id,
				timer->value);
	}
}
