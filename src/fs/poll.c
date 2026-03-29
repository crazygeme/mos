#include <lib/klib.h>
#include <fs/fs.h>
#include <fs/poll.h>
#include <ps/ps.h>
#include <hw/time.h>
#include <errno.h>
#include <config.h>

#define TICK_MS (1000 / HZ)

/* Check all fds, fill revents, return count of ready fds. */
static int poll_check(struct pollfd *fds, unsigned nfds)
{
	unsigned i;
	int ready = 0;

	for (i = 0; i < nfds; i++) {
		short revents = 0;
		int fd = fds[i].fd;
		short events = fds[i].events;

		if (fd < 0) {
			fds[i].revents = 0;
			continue;
		}
		if (events & (POLLIN | POLLPRI))
			if (fs_select(fd, FS_POLL_READ) == 0)
				revents |= POLLIN;
		if (events & POLLOUT)
			if (fs_select(fd, FS_POLL_WRITE) == 0)
				revents |= POLLOUT;
		if (fs_select(fd, FS_POLL_EXCEPT) == 0)
			revents |= POLLERR;
		fds[i].revents = revents;
		if (revents)
			ready++;
	}
	return ready;
}

/*
 * Register task on every polled fd that supports poll_wait.
 * Returns 1 if at least one fd lacks poll_wait support (needs fallback timer).
 */
static int poll_reg(struct pollfd *fds, unsigned nfds, task_struct *task)
{
	task_struct *cur = CURRENT_TASK();
	unsigned i;
	int has_unsupported = 0;

	for (i = 0; i < nfds; i++) {
		int fd = fds[i].fd;
		if (fd < 0 || fd >= (int)MAX_FD || !cur->fds[fd].used)
			continue;
		file *fp = cur->fds[fd].fp;
		if (!fp || !fp->f_fop)
			continue;
		if (fp->f_fop->poll_wait)
			fp->f_fop->poll_wait(fp, task);
		else
			has_unsupported = 1;
	}
	return has_unsupported;
}

/* Deregister task from every polled fd. */
static void poll_dereg(struct pollfd *fds, unsigned nfds, task_struct *task)
{
	task_struct *cur = CURRENT_TASK();
	unsigned i;

	for (i = 0; i < nfds; i++) {
		int fd = fds[i].fd;
		if (fd < 0 || fd >= (int)MAX_FD || !cur->fds[fd].used)
			continue;
		file *fp = cur->fds[fd].fp;
		if (fp && fp->f_fop && fp->f_fop->poll_wait_remove)
			fp->f_fop->poll_wait_remove(fp, task);
	}
}

int do_poll(struct pollfd *fds, unsigned nfds, int timeout)
{
	int ret = 0;
	int just_test = (timeout == 0);
	int infinite = (timeout < 0);
	unsigned deadline = 0;
	task_struct *cur = CURRENT_TASK();

	if (!fds && nfds > 0)
		return -EFAULT;

	if (!just_test && !infinite)
		deadline = time_now_ms() + (unsigned)timeout;

	for (;;) {
		/* Phase 1: check current readiness */
		ret = poll_check(fds, nfds);
		if (ret > 0)
			break;

		if (just_test)
			break;

		if (!infinite && time_now_ms() >= deadline)
			break;

		/* Phase 2: register wakeup on each fd */
		int has_unsupported = poll_reg(fds, nfds, cur);

		/* Phase 3: re-check to close the lost-wakeup window */
		ret = poll_check(fds, nfds);
		if (ret > 0) {
			poll_dereg(fds, nfds, cur);
			break;
		}

		/* Phase 4: sleep until an fd wakes us or the deadline expires.
		 * time_wait(0) blocks indefinitely — only an fd wakeup will
		 * unblock us.  For unsupported fds or finite timeouts we arm a
		 * timer so we periodically re-check. */
		unsigned sleep_ms = 0;
		if (!infinite) {
			unsigned now = time_now_ms();
			if (now >= deadline) {
				poll_dereg(fds, nfds, cur);
				ret = 0;
				break;
			}
			sleep_ms = deadline - now;
			if (has_unsupported && sleep_ms > TICK_MS)
				sleep_ms = TICK_MS;
		} else if (has_unsupported) {
			sleep_ms = TICK_MS;
		}

		time_wait(sleep_ms);
		poll_dereg(fds, nfds, cur);

		/* Signal check */
		if (cur->signal->sig_pending & ~cur->signal->sig_mask) {
			ret = -EINTR;
			break;
		}
	}

	return ret;
}
