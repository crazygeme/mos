#include <fs/epoll.h>
#include <fs/fcntl.h>
#include <fs/poll.h>
#include <lib/rbtree.h>
#include <ps/ps.h>
#include <device/time.h>
#include <macro.h>
#include <mm/mmap.h>
#include <errno.h>

#define EPOLL_DEPTH 5
#define EPOLL_CHANNELS 4
#define EPOLL_READ_BITS (EPOLLIN | EPOLLRDNORM)
#define EPOLL_WRITE_BITS (EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)
#define EPOLL_ALL                                                      \
	(FS_POLL_READ | FS_POLL_WRITE | FS_POLL_EXCEPT | FS_POLL_ERR | \
	 FS_POLL_HUP | FS_POLL_RDHUP)

struct eventpoll {
	file *fp;
	struct rb_root interests;
	list_entry ready, waiters;
	spinlock_t ready_lock, wait_lock;
	unsigned ready_count;
};
struct epitem {
	struct rb_node tree;
	list_entry ready;
	struct eventpoll *ep;
	file *fp;
	int fd, queued, armed;
	uint32_t events;
	uint64_t data;
	struct epitem *file_next, **file_prev;
	poll_table subscriptions;
	poll_table_entry channels[EPOLL_CHANNELS];
};
/* Serializes graph, interest, and file-lifetime changes. Producers only take
 * the ready and waiter spinlocks and never enter this mutex. */
static rmutex_t epoll_mutex;
static const file_operations epoll_fops;
static void epoll_init(void)
{
	rmutex_init(&epoll_mutex);
}
KERNEL_INIT(1, epoll_init);

/* Validate each actual transfer; maxevents need not describe mapped storage
 * beyond the records returned by the call. */
static int epoll_access(const void *ptr, size_t size, int write)
{
	vaddr_t addr = (vaddr_t)(uintptr_t)ptr, end;
	mm_struct *mm;
	if (!ptr)
		return 0;
	if (current->type != ps_user)
		return 1;
	mm = current->user->vm;
	if (addr >= mm->task_size || size > mm->task_size - addr)
		return 0;
	end = addr + size;
	while (addr < end) {
		vm_region *region = vm_find_map_cached(current->user, addr);
		if (!region ||
		    !(region->prot & (write ? PROT_WRITE : PROT_READ)))
			return 0;
		addr = region->end < end ? region->end : end;
	}
	return 1;
}

static file *epoll_get_fd(int fd)
{
	file *fp = NULL;
	if (fd < 0 || fd >= MAX_FD)
		return NULL;
	mutex_lock(&current->files->lock);
	fp = current->fds[fd];
	if (fp && !(fp->f_flag & O_PATH))
		fs_get_file(fp);
	else
		fp = NULL;
	mutex_unlock(&current->files->lock);
	return fp;
}

static void epoll_wake(struct eventpoll *ep)
{
	int irq;
	spinlock_lock(&ep->wait_lock, &irq);
	poll_notify(&ep->waiters);
	spinlock_unlock(&ep->wait_lock, irq);
}

static void epoll_queue(void *opaque)
{
	struct epitem *item = opaque;
	struct eventpoll *ep = item->ep;
	int irq, notify = 0;
	spinlock_lock(&ep->ready_lock, &irq);
	if (item->armed) {
		notify = 1;
		if (!item->queued) {
			list_insert_tail(&ep->ready, &item->ready);
			item->queued = 1;
			ep->ready_count++;
		}
	}
	spinlock_unlock(&ep->ready_lock, irq);
	if (notify)
		epoll_wake(ep);
}

static struct epitem *epoll_find(struct eventpoll *ep, file *fp, int fd,
				 struct rb_node ***slot,
				 struct rb_node **parent)
{
	struct rb_node **link = &ep->interests.rb_node, *above = NULL;
	while (*link) {
		struct epitem *item = rb_entry(*link, struct epitem, tree);
		above = *link;
		if ((uintptr_t)fp < (uintptr_t)item->fp ||
		    (fp == item->fp && fd < item->fd))
			link = &above->rb_left;
		else if (fp != item->fp || fd != item->fd)
			link = &above->rb_right;
		else
			return item;
	}
	if (slot)
		*slot = link;
	if (parent)
		*parent = above;
	return NULL;
}

static void epoll_unqueue(struct epitem *item)
{
	int irq;
	spinlock_lock(&item->ep->ready_lock, &irq);
	if (item->queued) {
		list_remove_entry(&item->ready);
		item->queued = 0;
		item->ep->ready_count--;
	}
	spinlock_unlock(&item->ep->ready_lock, irq);
}

static void epoll_remove(struct epitem *item)
{
	int irq;
	/* Stop callbacks before detaching or freeing the interest. */
	spinlock_lock(&item->ep->ready_lock, &irq);
	item->armed = 0;
	spinlock_unlock(&item->ep->ready_lock, irq);
	poll_table_cleanup(&item->subscriptions);
	epoll_unqueue(item);
	rb_erase(&item->tree, &item->ep->interests);
	*item->file_prev = item->file_next;
	if (item->file_next)
		item->file_next->file_prev = item->file_prev;
	free(item);
}

void epoll_release_file(file *fp)
{
	if (!fp->f_ep_links)
		return;
	rmutex_lock(&epoll_mutex);
	while (fp->f_ep_links)
		epoll_remove(fp->f_ep_links);
	rmutex_unlock(&epoll_mutex);
}

static unsigned epoll_mask(struct epitem *item)
{
	unsigned mask = item->fp->f_fop->poll(item->fp, EPOLL_ALL, NULL);
	unsigned result = 0;
	if (mask & FS_POLL_READ)
		result |= item->events & EPOLL_READ_BITS;
	if (mask & FS_POLL_WRITE)
		result |= item->events & EPOLL_WRITE_BITS;
	if (mask & FS_POLL_EXCEPT)
		result |= item->events & (EPOLLPRI | EPOLLRDBAND);
	if (mask & FS_POLL_ERR)
		result |= EPOLLERR;
	if (mask & FS_POLL_HUP)
		result |= EPOLLHUP;
	if (mask & FS_POLL_RDHUP)
		result |= item->events & EPOLLRDHUP;
	return result;
}

static struct epitem *epoll_pop(struct eventpoll *ep)
{
	struct epitem *item = NULL;
	int irq;
	spinlock_lock(&ep->ready_lock, &irq);
	if (ep->ready_count) {
		item = container_of(list_remove_head(&ep->ready), struct epitem,
				    ready);
		item->queued = 0;
		ep->ready_count--;
	}
	spinlock_unlock(&ep->ready_lock, irq);
	return item;
}

static unsigned epoll_ready_count(struct eventpoll *ep)
{
	int irq;
	unsigned count;
	spinlock_lock(&ep->ready_lock, &irq);
	count = ep->ready_count;
	spinlock_unlock(&ep->ready_lock, irq);
	return count;
}

/* Revalidation discards stale candidates without scanning idle interests. */
static int epoll_ready(struct eventpoll *ep)
{
	unsigned budget = epoll_ready_count(ep);
	while (budget--) {
		struct epitem *item = epoll_pop(ep);
		if (!item)
			break;
		if (item->armed && epoll_mask(item)) {
			/* Keep the candidate queued without producing another edge. */
			int irq;
			spinlock_lock(&ep->ready_lock, &irq);
			if (!item->queued) {
				list_insert_head(&ep->ready, &item->ready);
				item->queued = 1;
				ep->ready_count++;
			}
			spinlock_unlock(&ep->ready_lock, irq);
			return 1;
		}
	}
	return 0;
}

static unsigned epoll_poll(file *fp, unsigned events, poll_table *pt)
{
	struct eventpoll *ep = fp->f_inode->i_private;
	unsigned ready;
	if (pt)
		poll_subscribe(pt, &ep->waiters, &ep->wait_lock);
	rmutex_lock(&epoll_mutex);
	ready = (events & FS_POLL_READ) && epoll_ready(ep) ? FS_POLL_READ : 0;
	rmutex_unlock(&epoll_mutex);
	return ready;
}

static int epoll_release(file *fp)
{
	struct eventpoll *ep = fp->f_inode->i_private;
	struct rb_node *node;
	rmutex_lock(&epoll_mutex);
	while ((node = rb_first(&ep->interests)))
		epoll_remove(rb_entry(node, struct epitem, tree));
	rmutex_unlock(&epoll_mutex);
	free(ep);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int epoll_getattr(file *fp, struct stat *st)
{
	(void)fp;
	memset(st, 0, sizeof(*st));
	st->st_mode = 0600;
	st->st_nlink = 1;
	return 0;
}
static const file_operations epoll_fops = {
	.poll = epoll_poll,
	.release = epoll_release,
	.getattr = epoll_getattr,
};

int sys_epoll_create1(int flags)
{
	file *fp;
	struct eventpoll *ep;
	int fd;
	if (flags & ~O_CLOEXEC)
		return -EINVAL;
	fp = zalloc(sizeof(*fp));
	ep = zalloc(sizeof(*ep));
	if (!fp || !ep) {
		free(fp);
		free(ep);
		return -ENOMEM;
	}
	fp->f_inode = zalloc(sizeof(*fp->f_inode));
	if (!fp->f_inode) {
		free(ep);
		free(fp);
		return -ENOMEM;
	}
	fp->f_inode->i_private = ep;
	fp->f_fop = &epoll_fops;
	fp->f_count = 1;
	fp->f_mode = O_RDWR;
	ep->fp = fp;
	list_init(&ep->ready);
	list_init(&ep->waiters);
	spinlock_init(&ep->ready_lock);
	spinlock_init(&ep->wait_lock);
	fd = fs_install_fd(fp, flags);
	if (fd < 0) {
		fs_put_file(fp);
		return -EMFILE;
	}
	return fd;
}

int sys_epoll_create(int size)
{
	return size > 0 ? sys_epoll_create1(0) : -EINVAL;
}

/* Bound recursion before traversing the next edge. The graph is acyclic. */
static int epoll_down(struct eventpoll *ep, struct eventpoll *target, int depth)
{
	struct rb_node *node;
	int longest = 0;
	if (ep == target || depth >= EPOLL_DEPTH)
		return EPOLL_DEPTH;
	for (node = rb_first(&ep->interests); node; node = rb_next(node)) {
		struct epitem *item = rb_entry(node, struct epitem, tree);
		if (item->fp->f_fop == &epoll_fops) {
			int n = 1 + epoll_down(item->fp->f_inode->i_private,
					       target, depth + 1);
			if (n > longest)
				longest = n;
		}
	}
	return longest;
}
static int epoll_up(struct eventpoll *ep, int depth)
{
	struct epitem *item;
	int longest = 0;
	if (depth >= EPOLL_DEPTH)
		return EPOLL_DEPTH;
	for (item = ep->fp->f_ep_links; item; item = item->file_next) {
		int n = 1 + epoll_up(item->ep, depth + 1);
		if (n > longest)
			longest = n;
	}
	return longest;
}

int sys_epoll_ctl(int epfd, int op, int fd, const struct epoll_event *event)
{
	file *epfp = epoll_get_fd(epfd), *fp = NULL;
	struct eventpoll *ep;
	struct epitem *item;
	struct rb_node **slot, *parent;
	struct epoll_event value = { 0 };
	int result = -EBADF, irq;
	if (!epfp)
		return result;
	fp = epoll_get_fd(fd);
	if (!fp)
		goto done;
	result = -EINVAL;
	if (epfp->f_fop != &epoll_fops || epfp == fp ||
	    (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL))
		goto done;
	if (op != EPOLL_CTL_DEL) {
		result = -EFAULT;
		if (!epoll_access(event, sizeof(*event), 0))
			goto done;
		value = *event;
		result = -EINVAL;
		if (value.events & EPOLLEXCLUSIVE)
			goto done;
	}
	result = -EPERM;
	if (!fp->f_fop || !fp->f_fop->poll ||
	    (fp->f_inode &&
	     (S_ISREG(fp->f_inode->i_mode) || S_ISDIR(fp->f_inode->i_mode))))
		goto done;
	ep = epfp->f_inode->i_private;
	rmutex_lock(&epoll_mutex);
	item = epoll_find(ep, fp, fd, &slot, &parent);
	if (op == EPOLL_CTL_ADD) {
		result = -EEXIST;
		if (item)
			goto unlock;
		result = -ELOOP;
		if (fp->f_fop == &epoll_fops &&
		    epoll_up(ep, 0) + 1 +
				    epoll_down(fp->f_inode->i_private, ep, 0) >=
			    EPOLL_DEPTH)
			goto unlock;
		item = zalloc(sizeof(*item));
		result = -ENOMEM;
		if (!item)
			goto unlock;
		item->ep = ep;
		item->fp = fp;
		item->fd = fd;
		item->events = value.events;
		item->data = value.data;
		item->armed = 1;
		poll_table_init(&item->subscriptions, NULL, item->channels,
				EPOLL_CHANNELS);
		item->subscriptions.wake = epoll_queue;
		item->subscriptions.wake_arg = item;
		fp->f_fop->poll(fp, EPOLL_ALL, &item->subscriptions);
		if (item->subscriptions.unsupported ||
		    !item->subscriptions.nr) {
			poll_table_cleanup(&item->subscriptions);
			epoll_unqueue(item);
			free(item);
			result = -EPERM;
			goto unlock;
		}
		rb_link_node(&item->tree, parent, slot);
		rb_insert_color(&item->tree, &ep->interests);
		item->file_next = fp->f_ep_links;
		item->file_prev = &fp->f_ep_links;
		if (item->file_next)
			item->file_next->file_prev = &item->file_next;
		fp->f_ep_links = item;
	} else {
		result = -ENOENT;
		if (!item)
			goto unlock;
		if (op == EPOLL_CTL_DEL) {
			epoll_remove(item);
			result = 0;
			goto unlock;
		}
		spinlock_lock(&ep->ready_lock, &irq);
		item->events = value.events;
		item->data = value.data;
		item->armed = 1;
		spinlock_unlock(&ep->ready_lock, irq);
	}
	if (epoll_mask(item))
		epoll_queue(item);
	result = 0;
unlock:
	rmutex_unlock(&epoll_mutex);
done:
	if (fp)
		fs_put_file(fp);
	fs_put_file(epfp);
	return result;
}

struct epoll_wait_context {
	file *fp;
	struct eventpoll *ep;
	struct epoll_event *events;
	int maxevents;
	poll_table table;
	poll_table_entry entry;
};
static int epoll_deliver(void *opaque)
{
	struct epoll_wait_context *ctx = opaque;
	struct eventpoll *ep = ctx->ep;
	unsigned budget;
	int count = 0;
	rmutex_lock(&epoll_mutex);
	budget = epoll_ready_count(ep);
	while (budget-- && count < ctx->maxevents) {
		struct epitem *item = epoll_pop(ep);
		unsigned mask;
		if (!item)
			break;
		if (!item->armed || !(mask = epoll_mask(item)))
			continue;
		if (!epoll_access(&ctx->events[count], sizeof(*ctx->events),
				  1)) {
			epoll_queue(item);
			if (!count)
				count = -EFAULT;
			break;
		}
		ctx->events[count].events = mask;
		ctx->events[count++].data = item->data;
		if (item->events & EPOLLONESHOT) {
			int irq;
			spinlock_lock(&ep->ready_lock, &irq);
			item->armed = 0;
			spinlock_unlock(&ep->ready_lock, irq);
			epoll_unqueue(item);
		} else if (!(item->events & EPOLLET)) {
			int irq;
			spinlock_lock(&ep->ready_lock, &irq);
			if (!item->queued) {
				list_insert_tail(&ep->ready, &item->ready);
				item->queued = 1;
				ep->ready_count++;
			}
			spinlock_unlock(&ep->ready_lock, irq);
		}
	}
	rmutex_unlock(&epoll_mutex);
	return count;
}
static void epoll_wait_cancel(void *opaque)
{
	struct epoll_wait_context *ctx = opaque;
	poll_table_cleanup(&ctx->table);
	/* The cancellation caller has already detached the owning task. */
	fs_put_file(ctx->fp);
}
static int epoll_wait_register(void *opaque)
{
	struct epoll_wait_context *ctx = opaque;
	poll_subscribe(&ctx->table, &ctx->ep->waiters, &ctx->ep->wait_lock);
	current->io_wait = ctx;
	current->cancel_io_wait = epoll_wait_cancel;
	return ctx->table.unsupported;
}
static void epoll_wait_unregister(void *opaque)
{
	struct epoll_wait_context *ctx = opaque;
	poll_table_cleanup(&ctx->table);
	if (current->io_wait == ctx) {
		current->io_wait = NULL;
		current->cancel_io_wait = NULL;
	}
}
static const struct poll_ops epoll_wait_ops = {
	.check = epoll_deliver,
	.reg = epoll_wait_register,
	.dereg = epoll_wait_unregister,
};

static int epoll_wait_common(int epfd, struct epoll_event *events,
			     int maxevents, int just_test, int infinite,
			     unsigned long long deadline, const sigset_t *mask,
			     unsigned masksize)
{
	struct epoll_wait_context ctx;
	sigset_t saved_mask;
	int result;
	if (maxevents <= 0 ||
	    (unsigned)maxevents > 0x7fffffffU / sizeof(*events))
		return -EINVAL;
	if (current->type == ps_user) {
		vaddr_t addr = (vaddr_t)(uintptr_t)events;
		vaddr_t limit = current->user->vm->task_size;
		if (addr >= limit ||
		    (size_t)maxevents * sizeof(*events) > limit - addr)
			return -EFAULT;
	}
	if (mask && masksize != 8)
		return -EINVAL;
	if (mask && !epoll_access(mask, 8, 0))
		return -EFAULT;
	ctx.fp = epoll_get_fd(epfd);
	if (!ctx.fp)
		return -EBADF;
	if (ctx.fp->f_fop != &epoll_fops) {
		fs_put_file(ctx.fp);
		return -EINVAL;
	}
	ctx.ep = ctx.fp->f_inode->i_private;
	ctx.events = events;
	ctx.maxevents = maxevents;
	poll_table_init(&ctx.table, current, &ctx.entry, 1);
	saved_mask = current->signal->sig_mask;
	if (mask)
		current->signal->sig_mask = *mask & ~((1UL << (SIGKILL - 1)) |
						      (1UL << (SIGSTOP - 1)));
	result = poll_wait_loop(&epoll_wait_ops, &ctx, just_test, infinite,
				deadline);
	if (mask) {
		if (result == -EINTR) {
			current->signal->saved_sigmask = saved_mask;
			current->signal->restore_sigmask = 1;
		} else
			current->signal->sig_mask = saved_mask;
	}
	fs_put_file(ctx.fp);
	return result;
}
int sys_epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
		    int timeout, const sigset_t *mask, unsigned masksize)
{
	unsigned long long deadline =
		timeout > 0 ? time_deadline_ms((unsigned)timeout) : 0;
	return epoll_wait_common(epfd, events, maxevents, !timeout, timeout < 0,
				 deadline, mask, masksize);
}
int sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents,
		   int timeout)
{
	return sys_epoll_pwait(epfd, events, maxevents, timeout, NULL, 0);
}
int sys_epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
		     const struct epoll_timespec64 *timeout,
		     const sigset_t *mask, unsigned masksize)
{
	unsigned long long deadline = 0, ms;
	int just_test = 0;
	if (timeout) {
		if (!epoll_access(timeout, sizeof(*timeout), 0))
			return -EFAULT;
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
		    timeout->tv_nsec >= 1000000000)
			return -EINVAL;
		just_test = !timeout->tv_sec && !timeout->tv_nsec;
		ms = (unsigned long long)timeout->tv_nsec / 1000000 +
		     ((unsigned long long)timeout->tv_nsec % 1000000 != 0);
		/* Saturate deadlines so large time64 intervals remain finite. */
		if ((unsigned long long)timeout->tv_sec >
		    (0x7fffffffffffffffULL - ms) / 1000)
			deadline = 0x7fffffffffffffffULL;
		else
			deadline = time_deadline_ms(
				(unsigned long long)timeout->tv_sec * 1000 + ms);
	}
	return epoll_wait_common(epfd, events, maxevents, just_test, !timeout,
				 deadline, mask, masksize);
}
