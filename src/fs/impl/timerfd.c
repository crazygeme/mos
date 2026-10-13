#include <fs/anonfd.h>
#include <fs/timerfd.h>
#include <fs/fcntl.h>
#include <fs/poll.h>
#include <syscall/syscall.h>
#include <ps/ps.h>
#include <errno.h>

#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2
#define NS_PER_SEC 1000000000ULL

struct timerfd_state {
	list_entry list, waiters;
	uint64_t due, interval, ticks;
	unsigned generation;
	int clockid, wall, cancel_on_set, canceled;
};
/* This lock serializes expiration, rearming, read, and final release. */
static spinlock_t timerfd_lock = SPINLOCK_INITIALIZER;
static list_entry timerfds;
static const file_operations timerfd_fops;
static void timerfd_init(void)
{
	list_init(&timerfds);
}
KERNEL_INIT(1, timerfd_init);

static uint64_t timerfd_now(struct timerfd_state *state, unsigned *generation)
{
	if (state->wall)
		return time_wall_snapshot_us(generation) * 1000ULL;
	return time_now_us() * 1000ULL;
}
static void timerfd_update(struct timerfd_state *state)
{
	unsigned generation = state->generation;
	uint64_t now = timerfd_now(state, &generation);
	if (state->cancel_on_set && generation != state->generation) {
		state->generation = generation;
		state->canceled = 1;
		state->ticks = 0;
		state->due = 0;
		poll_notify(&state->waiters);
		return;
	}
	if (!state->due || state->due > now)
		return;
	uint64_t count = 1;
	if (state->interval) {
		count += (now - state->due) / state->interval;
		/* Keep the next deadline strictly later than now without overflow. */
		uint64_t remaining =
			state->interval - (now - state->due) % state->interval;
		state->due = remaining > UINT64_MAX - now ? 0 : now + remaining;
	} else {
		state->due = 0;
	}
	state->ticks = count > UINT64_MAX - state->ticks ? UINT64_MAX :
							   state->ticks + count;
	poll_notify(&state->waiters);
}
void timerfd_poll_timers(void)
{
	int irq;
	spinlock_lock(&timerfd_lock, &irq);
	for (list_entry *entry = timerfds.next; entry != &timerfds;
	     entry = entry->next)
		timerfd_update(container_of(entry, struct timerfd_state, list));
	spinlock_unlock(&timerfd_lock, irq);
}
static unsigned timerfd_poll(file *fp, unsigned events, poll_table *pt)
{
	struct timerfd_state *state = fp->f_inode->i_private;
	int irq;
	if (pt)
		poll_subscribe(pt, &state->waiters, &timerfd_lock);
	spinlock_lock(&timerfd_lock, &irq);
	timerfd_update(state);
	unsigned ready =
		state->ticks || state->canceled ? (events & FS_POLL_READ) : 0;
	spinlock_unlock(&timerfd_lock, irq);
	return ready;
}
struct timerfd_read {
	struct timerfd_state *state;
	uint64_t ticks;
};
static int timerfd_take(void *opaque)
{
	struct timerfd_read *read = opaque;
	struct timerfd_state *state = read->state;
	int irq, ret = 0;
	spinlock_lock(&timerfd_lock, &irq);
	timerfd_update(state);
	if (state->canceled) {
		state->canceled = 0;
		ret = -ECANCELED;
	} else if (state->ticks) {
		read->ticks = state->ticks;
		state->ticks = 0;
		ret = sizeof(read->ticks);
	}
	spinlock_unlock(&timerfd_lock, irq);
	return ret;
}
static ssize_t timerfd_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	struct timerfd_read read = { .state = fp->f_inode->i_private };
	if (count < sizeof(read.ticks))
		return -EINVAL;
	int ret = anonfd_read_wait(fp, timerfd_take, &read);
	if (ret > 0 && ps_write_process_memory(current, buf, &read.ticks,
					       sizeof(read.ticks)) < 0)
		return -EFAULT;
	return ret;
}
static int timerfd_release(file *fp)
{
	struct timerfd_state *state = fp->f_inode->i_private;
	int irq;
	spinlock_lock(&timerfd_lock, &irq);
	list_remove_entry(&state->list);
	spinlock_unlock(&timerfd_lock, irq);
	free(state);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static const file_operations timerfd_fops = {
	.read = timerfd_read,
	.poll = timerfd_poll,
	.getattr = anonfd_getattr,
	.llseek = anonfd_llseek,
	.release = timerfd_release,
};
int sys_timerfd_create(int clockid, int flags)
{
	if ((clockid != 0 && clockid != 1 && clockid != 7) ||
	    flags & ~(O_NONBLOCK | O_CLOEXEC))
		return -EINVAL;
	struct timerfd_state *state = zalloc(sizeof(*state));
	if (!state)
		return -ENOMEM;
	state->clockid = clockid;
	list_init(&state->waiters);
	int irq;
	spinlock_lock(&timerfd_lock, &irq);
	list_insert_tail(&timerfds, &state->list);
	spinlock_unlock(&timerfd_lock, irq);
	int fd = anonfd_install(state, &timerfd_fops, flags);
	if (fd < 0) {
		spinlock_lock(&timerfd_lock, &irq);
		list_remove_entry(&state->list);
		spinlock_unlock(&timerfd_lock, irq);
		free(state);
	}
	return fd;
}
static int timerfd_ns(int64_t sec, int64_t nsec, uint64_t *ns)
{
	if (sec < 0 || nsec < 0 || nsec >= NS_PER_SEC ||
	    (uint64_t)sec > (UINT64_MAX - (uint64_t)nsec) / NS_PER_SEC)
		return -EINVAL;
	*ns = (uint64_t)sec * NS_PER_SEC + nsec;
	return 0;
}
static void timerfd_get(struct timerfd_state *state, struct timerfd_spec *value)
{
	timerfd_update(state);
	uint64_t now = timerfd_now(state, NULL);
	uint64_t left = state->due > now ? state->due - now : 0;
	*value = (struct timerfd_spec){
		.interval_sec = state->interval / NS_PER_SEC,
		.interval_nsec = state->interval % NS_PER_SEC,
		.value_sec = left / NS_PER_SEC,
		.value_nsec = left % NS_PER_SEC
	};
}
static int timerfd_control(int fd, int flags, const struct timerfd_spec *value,
			   struct timerfd_spec *old)
{
	uint64_t due = 0, interval = 0;
	if (value && (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)))
		return -EINVAL;
	if (value) {
		int ret = timerfd_ns(value->value_sec, value->value_nsec, &due);
		if (ret)
			return ret;
		ret = timerfd_ns(value->interval_sec, value->interval_nsec,
				 &interval);
		if (ret)
			return ret;
	}
	file *fp = anonfd_get(fd);
	if (!fp)
		return -EBADF;
	if (fp->f_fop != &timerfd_fops) {
		fs_put_file(fp);
		return -EINVAL;
	}
	struct timerfd_state *state = fp->f_inode->i_private;
	int irq, ret = 0;
	spinlock_lock(&timerfd_lock, &irq);
	if (old)
		timerfd_get(state, old);
	if (value) {
		int wall = state->clockid == 0 && (flags & TFD_TIMER_ABSTIME);
		unsigned generation = 0;
		uint64_t now =
			wall ? time_wall_snapshot_us(&generation) * 1000ULL :
			       time_now_us() * 1000ULL;
		if (!(flags & TFD_TIMER_ABSTIME) && due > UINT64_MAX - now) {
			ret = -EINVAL;
		} else {
			state->wall = wall;
			state->generation = generation;
			state->cancel_on_set =
				wall && (flags & TFD_TIMER_CANCEL_ON_SET);
			state->canceled = 0;
			state->ticks = 0;
			state->interval = interval;
			state->due = due ? ((flags & TFD_TIMER_ABSTIME) ?
						    due :
						    now + due) :
					   0;
			timerfd_update(state);
		}
	}
	spinlock_unlock(&timerfd_lock, irq);
	fs_put_file(fp);
	return ret;
}
int sys_timerfd_settime64(int fd, int flags, const void *input, void *output)
{
	struct timerfd_spec value, old;
	if (ps_read_process_memory(current, input, &value, sizeof(value)) < 0)
		return -EFAULT;
	int ret = timerfd_control(fd, flags, &value, output ? &old : NULL);
	if (!ret && output &&
	    ps_write_process_memory(current, output, &old, sizeof(old)) < 0)
		return -EFAULT;
	return ret;
}
int sys_timerfd_gettime64(int fd, void *output)
{
	struct timerfd_spec value;
	int ret = timerfd_control(fd, 0, NULL, &value);
	if (!ret &&
	    ps_write_process_memory(current, output, &value, sizeof(value)) < 0)
		return -EFAULT;
	return ret;
}
/* The legacy i386 ABI consists of four signed 32-bit fields. */
struct timerfd_spec32 {
	int32_t interval_sec, interval_nsec, value_sec, value_nsec;
};
static int timerfd_copy32(void *output, const struct timerfd_spec *value)
{
	if (value->interval_sec > 0x7fffffff || value->value_sec > 0x7fffffff)
		return -EOVERFLOW;
	struct timerfd_spec32 wire = { value->interval_sec,
				       value->interval_nsec, value->value_sec,
				       value->value_nsec };
	return ps_write_process_memory(current, output, &wire, sizeof(wire)) <
			       0 ?
		       -EFAULT :
		       0;
}
int sys_timerfd_settime(int fd, int flags, const void *input, void *output)
{
	struct timerfd_spec32 wire;
	if (ps_read_process_memory(current, input, &wire, sizeof(wire)) < 0)
		return -EFAULT;
	struct timerfd_spec value = { wire.interval_sec, wire.interval_nsec,
				      wire.value_sec, wire.value_nsec },
			    old;
	int ret = timerfd_control(fd, flags, &value, output ? &old : NULL);
	return !ret && output ? timerfd_copy32(output, &old) : ret;
}
int sys_timerfd_gettime(int fd, void *output)
{
	struct timerfd_spec value;
	int ret = timerfd_control(fd, 0, NULL, &value);
	return ret ? ret : timerfd_copy32(output, &value);
}
