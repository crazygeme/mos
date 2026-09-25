#include <ps/ps.h>
#include <hw/time.h>
#include <errno.h>
#include <lib/klib.h>
#include "syscall_internal.h"

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
	int notify;
	int signo;
	int value;
	int overrun;
	unsigned long long due_ns;
	unsigned long long interval_ns;
};

static struct mos_timer timers[MOS_TIMER_COUNT];
static int next_timer_id = 1;

static unsigned long long timer_now_ns(int clockid)
{
	return (clockid == 0 ? time_wall_us() : time_now_us()) * 1000ULL;
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
	int i;
	for (i = 0; i < MOS_TIMER_COUNT; ++i)
		if (timers[i].id == id && timers[i].owner == current->tgid)
			return &timers[i];
	return NULL;
}

int sys_timer_create(int clockid, const struct mos_sigevent *event, int *timerid)
{
	struct mos_timer *timer = NULL;
	int i;
	int notify = event ? event->notify : MOS_SIGEV_SIGNAL;
	int signo = event ? event->signo : SIGALRM;

	if (!timerid)
		return -EFAULT;
	if (clockid != 0 && clockid != 1)
		return -EINVAL;
	if (notify != MOS_SIGEV_NONE && notify != MOS_SIGEV_SIGNAL &&
	    notify != (MOS_SIGEV_SIGNAL | MOS_SIGEV_THREAD_ID))
		return -EINVAL;
	if (notify != MOS_SIGEV_NONE &&
	    (signo < 1 || signo > SIGRTMIN_KERNEL))
		return -EINVAL;
	if ((notify & MOS_SIGEV_THREAD_ID) &&
	    (!ps_find_process((unsigned)event->tid) ||
	     ps_find_process((unsigned)event->tid)->tgid != current->tgid))
		return -EINVAL;
	for (i = 0; i < MOS_TIMER_COUNT; ++i)
		if (!timers[i].id) {
			timer = &timers[i];
			break;
		}
	if (!timer)
		return -EAGAIN;
	memset(timer, 0, sizeof(*timer));
	timer->owner = current->tgid;
	timer->target = event && (notify & MOS_SIGEV_THREAD_ID) ?
			(unsigned)event->tid : current->psid;
	timer->clockid = clockid;
	timer->notify = notify;
	timer->signo = signo;
	timer->value = event ? event->value : 0;
	timer->id = next_timer_id++;
	if (next_timer_id <= 0)
		next_timer_id = 1;
	*timerid = timer->id;
	return 0;
}

int sys_timer_settime(int timerid, int flags,
		      const struct mos_itimerspec *value,
		      struct mos_itimerspec *old_value)
{
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
	now = timer_now_ns(timer->clockid);
	if (old_value) {
		timer_ns_timespec(timer->interval_ns, &old_value->it_interval);
		timer_ns_timespec(timer->due_ns > now ? timer->due_ns - now : 0,
				   &old_value->it_value);
	}
	timer->interval_ns = interval;
	timer->due_ns = due ? ((flags & MOS_TIMER_ABSTIME) ? due : now + due) : 0;
	timer->overrun = 0;
	return 0;
}

int sys_timer_gettime(int timerid, struct mos_itimerspec *value)
{
	struct mos_timer *timer = timer_lookup(timerid);
	unsigned long long now;
	if (!timer)
		return -EINVAL;
	if (!value)
		return -EFAULT;
	now = timer_now_ns(timer->clockid);
	timer_ns_timespec(timer->interval_ns, &value->it_interval);
	timer_ns_timespec(timer->due_ns > now ? timer->due_ns - now : 0,
			   &value->it_value);
	return 0;
}

int sys_timer_getoverrun(int timerid)
{
	struct mos_timer *timer = timer_lookup(timerid);
	return timer ? timer->overrun : -EINVAL;
}

int sys_timer_delete(int timerid)
{
	struct mos_timer *timer = timer_lookup(timerid);
	if (!timer)
		return -EINVAL;
	memset(timer, 0, sizeof(*timer));
	return 0;
}

void ps_timer_discard_group(unsigned tgid)
{
	int i;
	for (i = 0; i < MOS_TIMER_COUNT; ++i)
		if (timers[i].owner == tgid)
			memset(&timers[i], 0, sizeof(timers[i]));
}

void ps_timer_poll(void)
{
	int i;
	for (i = 0; i < MOS_TIMER_COUNT; ++i) {
		struct mos_timer *timer = &timers[i];
		unsigned long long now;
		if (!timer->id || !timer->due_ns)
			continue;
		now = timer_now_ns(timer->clockid);
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
		ps_timer_notify(timer->target, timer->signo,
				timer->id, timer->value);
	}
}
