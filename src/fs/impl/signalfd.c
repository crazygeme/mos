#include <fs/anonfd.h>
#include <fs/fcntl.h>
#include <fs/poll.h>
#include <syscall/syscall.h>
#include <ps/ps.h>
#include <errno.h>

struct signalfd_state {
	sigset_t mask;
};
/* Linux uses a fixed 128-byte record for every supported syscall ABI. */
struct signalfd_info {
	uint32_t signo;
	int32_t error, code;
	uint32_t pid, uid;
	int32_t fd;
	uint32_t tid, band, overrun, trapno;
	int32_t status, value_int;
	uint64_t value_ptr, utime, stime, address;
	unsigned char padding[48];
};
_Static_assert(sizeof(struct signalfd_info) == 128, "signalfd ABI");

struct signalfd_watch {
	list_entry list, waiters;
	poll_table_entry event;
	struct signalfd_state *state;
	task_thread *group;
	unsigned tid;
	uint64_t sequence;
};
static list_entry watches;
static spinlock_t watch_lock = SPINLOCK_INITIALIZER;
static const file_operations signalfd_fops;
static void signalfd_init(void)
{
	list_init(&watches);
}
KERNEL_INIT(1, signalfd_init);

static void signalfd_unwatch(void *opaque, task_struct *task)
{
	struct signalfd_watch *watch = opaque;
	int irq;
	spinlock_lock(&watch_lock, &irq);
	list_remove_entry(&watch->list);
	spinlock_unlock(&watch_lock, irq);
	ref_count_put(watch->group);
	free(watch);
}

/* Signal enqueue holds ps_lock. Publication runs in process context to avoid
 * acquiring scheduler and epoll waiter locks from inside signal enqueue. */
void signalfd_poll_signals(void)
{
	int irq;
	spinlock_lock(&watch_lock, &irq);
	for (list_entry *entry = watches.next; entry != &watches;
	     entry = entry->next) {
		struct signalfd_watch *watch =
			container_of(entry, struct signalfd_watch, list);
		uint64_t sequence;
		unsigned pending =
			ps_signalfd_snapshot(watch->tid, watch->group,
					     watch->state->mask, &sequence);
		if (pending && sequence != watch->sequence)
			poll_notify(&watch->waiters);
		watch->sequence = sequence;
	}
	spinlock_unlock(&watch_lock, irq);
}

static unsigned signalfd_poll(file *fp, unsigned events, poll_table *pt)
{
	struct signalfd_state *state = fp->f_inode->i_private;
	if (pt) {
		struct signalfd_watch *watch = zalloc(sizeof(*watch));
		if (!watch) {
			pt->unsupported = 1;
		} else {
			watch->state = state;
			watch->group = ref_count_get(current->thread);
			watch->tid = current->life->psid;
			watch->event.task = pt->task;
			watch->event.wake = pt->wake;
			watch->event.wake_arg = pt->wake_arg;
			list_init(&watch->waiters);
			list_insert_tail(&watch->waiters, &watch->event.node);
			ps_signalfd_snapshot(watch->tid, watch->group,
					     __atomic_load_n(&state->mask,
							     __ATOMIC_ACQUIRE),
					     &watch->sequence);
			if (poll_table_add(pt, watch, signalfd_unwatch) < 0) {
				ref_count_put(watch->group);
				free(watch);
			} else {
				int irq;
				spinlock_lock(&watch_lock, &irq);
				list_insert_tail(&watches, &watch->list);
				spinlock_unlock(&watch_lock, irq);
			}
		}
	}
	return (ps_pending_signals(current) &
		__atomic_load_n(&state->mask, __ATOMIC_ACQUIRE)) ?
		       (events & FS_POLL_READ) :
		       0;
}

struct signalfd_read {
	struct signalfd_state *state;
	struct signalfd_info info;
};
static int signalfd_take(void *opaque)
{
	struct signalfd_read *read = opaque;
	struct mos_signal_info info;
	int sig = ps_take_signal_info(
		current, __atomic_load_n(&read->state->mask, __ATOMIC_ACQUIRE),
		&info);
	if (!sig)
		return 0;
	memset(&read->info, 0, sizeof(read->info));
	read->info.signo = sig;
	read->info.code = info.code;
	read->info.pid = info.pid;
	read->info.uid = info.uid;
	read->info.tid = info.timer_id >= 0 ? info.timer_id : 0;
	read->info.overrun = info.overrun;
	read->info.status = info.status;
	read->info.value_int = (int32_t)info.value;
	read->info.value_ptr = info.value;
	read->info.utime = info.utime;
	read->info.stime = info.stime;
	return sizeof(read->info);
}
static ssize_t signalfd_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	struct signalfd_read read = { .state = fp->f_inode->i_private };
	unsigned done = 0;
	if (count < sizeof(read.info))
		return -EINVAL;
	int ret = anonfd_read_wait(fp, signalfd_take, &read);
	while (ret > 0) {
		if (ps_write_process_memory(current, (char *)buf + done,
					    &read.info, sizeof(read.info)) < 0)
			return done ? (int)done : -EFAULT;
		done += sizeof(read.info);
		if (count - done < sizeof(read.info))
			break;
		ret = signalfd_take(&read);
	}
	return done ? (int)done : ret;
}
static int signalfd_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static const file_operations signalfd_fops = {
	.read = signalfd_read,
	.poll = signalfd_poll,
	.getattr = anonfd_getattr,
	.llseek = anonfd_llseek,
	.release = signalfd_release,
};
int sys_signalfd4(int fd, const void *mask, size_t size, int flags)
{
	uint64_t input;
	if (size != sizeof(input) || flags & ~(O_NONBLOCK | O_CLOEXEC))
		return -EINVAL;
	if (ps_read_process_memory(current, mask, &input, sizeof(input)) < 0)
		return -EFAULT;
	sigset_t signals = (sigset_t)input &
			   ~((1U << (SIGKILL - 1)) | (1U << (SIGSTOP - 1)));
	if (fd != -1) {
		file *fp = anonfd_get(fd);
		if (!fp)
			return -EBADF;
		if (fp->f_fop != &signalfd_fops) {
			fs_put_file(fp);
			return -EINVAL;
		}
		struct signalfd_state *state = fp->f_inode->i_private;
		int irq;
		spinlock_lock(&watch_lock, &irq);
		__atomic_store_n(&state->mask, signals, __ATOMIC_RELEASE);
		for (list_entry *entry = watches.next; entry != &watches;
		     entry = entry->next) {
			struct signalfd_watch *watch = container_of(
				entry, struct signalfd_watch, list);
			if (watch->state == state)
				poll_notify(&watch->waiters);
		}
		spinlock_unlock(&watch_lock, irq);
		fs_put_file(fp);
		return fd;
	}
	struct signalfd_state *state = zalloc(sizeof(*state));
	if (!state)
		return -ENOMEM;
	state->mask = signals;
	fd = anonfd_install(state, &signalfd_fops, flags);
	if (fd < 0)
		free(state);
	return fd;
}
