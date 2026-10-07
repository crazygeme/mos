#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/poll.h>
#include <syscall/syscall.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <errno.h>

#define EFD_SEMAPHORE 1
#define EVENTFD_MAX (UINT64_MAX - 1)

typedef struct {
	uint64_t counter;
	int semaphore;
	unsigned uid, gid;
	spinlock_t lock;
	list_entry waiters;
} eventfd_state;

typedef struct {
	file *fp;
	eventfd_state *state;
	uint64_t value;
	int write;
	poll_table table;
	poll_table_entry entry;
} eventfd_wait;

/* Counter transfer and readiness publication share the producer lock. */
static int eventfd_transfer(void *opaque)
{
	eventfd_wait *wait = opaque;
	eventfd_state *state = wait->state;
	int irq;
	spinlock_lock(&state->lock, &irq);
	if (wait->write) {
		if (wait->value > EVENTFD_MAX - state->counter) {
			spinlock_unlock(&state->lock, irq);
			return 0;
		}
		state->counter += wait->value;
	} else {
		if (!state->counter) {
			spinlock_unlock(&state->lock, irq);
			return 0;
		}
		wait->value = state->semaphore ? 1 : state->counter;
		state->counter -= wait->value;
	}
	poll_notify(&state->waiters);
	spinlock_unlock(&state->lock, irq);
	return sizeof(uint64_t);
}

static void eventfd_wait_cancel(void *opaque)
{
	eventfd_wait *wait = opaque;
	poll_table_cleanup(&wait->table);
	fs_put_file(wait->fp);
}

static int eventfd_wait_register(void *opaque)
{
	eventfd_wait *wait = opaque;
	poll_subscribe(&wait->table, &wait->state->waiters, &wait->state->lock);
	current->io_wait = wait;
	current->cancel_io_wait = eventfd_wait_cancel;
	return wait->table.unsupported;
}

static void eventfd_wait_unregister(void *opaque)
{
	eventfd_wait *wait = opaque;
	poll_table_cleanup(&wait->table);
	if (current->io_wait == wait) {
		current->io_wait = NULL;
		current->cancel_io_wait = NULL;
	}
}

static const struct poll_ops eventfd_wait_ops = {
	.check = eventfd_transfer,
	.reg = eventfd_wait_register,
	.dereg = eventfd_wait_unregister,
};

static int eventfd_transfer_wait(eventfd_wait *wait)
{
	int result;
	fs_get_file(wait->fp);
	poll_table_init(&wait->table, current, &wait->entry, 1);
	result = poll_wait_loop(&eventfd_wait_ops, wait,
				!!(wait->fp->f_flag & O_NONBLOCK), 1, 0);
	fs_put_file(wait->fp);
	return result ? result : -EAGAIN;
}

static ssize_t eventfd_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	eventfd_wait wait = { .fp = fp, .state = fp->f_inode->i_private };
	int result;
	if (count < sizeof(wait.value))
		return -EINVAL;
	result = eventfd_transfer_wait(&wait);
	if (result > 0 && ps_write_process_memory(current, buf, &wait.value,
						  sizeof(wait.value)) < 0)
		return -EFAULT;
	return result;
}

static ssize_t eventfd_write(file *fp, const void *buf, size_t count,
			     loff_t *pos)
{
	eventfd_wait wait = { .fp = fp,
			      .state = fp->f_inode->i_private,
			      .write = 1 };
	if (count != sizeof(wait.value))
		return -EINVAL;
	if (ps_read_process_memory(current, buf, &wait.value,
				   sizeof(wait.value)) < 0)
		return -EFAULT;
	if (wait.value == UINT64_MAX)
		return -EINVAL;
	return eventfd_transfer_wait(&wait);
}

static unsigned eventfd_poll(file *fp, unsigned events, poll_table *pt)
{
	eventfd_state *state = fp->f_inode->i_private;
	unsigned ready = 0;
	int irq;
	if (pt)
		poll_subscribe(pt, &state->waiters, &state->lock);
	spinlock_lock(&state->lock, &irq);
	if (state->counter)
		ready |= events & FS_POLL_READ;
	if (state->counter < EVENTFD_MAX)
		ready |= events & FS_POLL_WRITE;
	spinlock_unlock(&state->lock, irq);
	return ready;
}

static loff_t eventfd_llseek(file *fp, loff_t offset, int whence)
{
	return fp->f_pos;
}

static int eventfd_getattr(file *fp, struct stat *st)
{
	eventfd_state *state = fp->f_inode->i_private;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IRUSR | S_IWUSR;
	st->st_nlink = 1;
	st->st_uid = state->uid;
	st->st_gid = state->gid;
	return 0;
}

static int eventfd_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations eventfd_fops = {
	.read = eventfd_read,
	.write = eventfd_write,
	.poll = eventfd_poll,
	.llseek = eventfd_llseek,
	.getattr = eventfd_getattr,
	.release = eventfd_release,
};

int sys_eventfd2(unsigned initval, int flags)
{
	eventfd_state *state;
	file *fp;
	inode *node;
	int fd;
	if (flags & ~(EFD_SEMAPHORE | O_NONBLOCK | O_CLOEXEC))
		return -EINVAL;
	state = zalloc(sizeof(*state));
	fp = zalloc(sizeof(*fp));
	node = zalloc(sizeof(*node));
	if (!state || !fp || !node) {
		free(state);
		free(fp);
		free(node);
		return -ENOMEM;
	}
	state->counter = initval;
	state->semaphore = !!(flags & EFD_SEMAPHORE);
	state->uid = current->user->euid;
	state->gid = current->user->egid;
	spinlock_init(&state->lock);
	list_init(&state->waiters);
	node->i_private = state;
	node->i_mode = S_IRUSR | S_IWUSR;
	fp->f_inode = node;
	fp->f_fop = &eventfd_fops;
	fp->f_count = 1;
	fp->f_mode = O_RDWR;
	fp->f_flag = O_RDWR | (flags & O_NONBLOCK);
	fd = fs_install_fd(fp, flags);
	if (fd < 0) {
		fs_put_file(fp);
		return fd < -1 ? fd : -EMFILE;
	}
	return fd;
}

int sys_eventfd(unsigned initval)
{
	return sys_eventfd2(initval, 0);
}
