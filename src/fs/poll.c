#include <lib/klib.h>
#include <fs/fs.h>
#include <fs/poll.h>
#include <ps/ps.h>
#include <hw/time.h>
#include <errno.h>
#include <config.h>

#define TICK_MS (1000 / HZ)

int do_poll(struct pollfd *fds, unsigned nfds, int timeout)
{
	int ret = 0;
	unsigned i;
	int just_test = 0;
	int infinit_wait = 0;
	unsigned deadline = 0;

	if (!fds && nfds > 0)
		return -EFAULT;

	if (timeout == 0) {
		just_test = 1;
	} else if (timeout < 0) {
		infinit_wait = 1;
	} else {
		deadline = time_now_ms() + (unsigned)timeout;
	}

	do {
		int ready = 0;

		for (i = 0; i < nfds; i++) {
			short revents = 0;
			int fd = fds[i].fd;
			short events = fds[i].events;

			if (fd < 0) {
				fds[i].revents = 0;
				continue;
			}

			if (events & (POLLIN | POLLPRI)) {
				if (fs_select(fd, FS_POLL_READ) == 0)
					revents |= POLLIN;
			}

			if (events & POLLOUT) {
				if (fs_select(fd, FS_POLL_WRITE) == 0)
					revents |= POLLOUT;
			}

			if (fs_select(fd, FS_POLL_EXCEPT) == 0)
				revents |= POLLERR;

			fds[i].revents = revents;
			if (revents)
				ready++;
		}

		if (ready > 0) {
			ret = ready;
			break;
		}

		if (just_test) {
			ret = 0;
			break;
		}

		if (!infinit_wait) {
			unsigned now = time_now_ms();
			unsigned sleep_ms;

			if (now >= deadline) {
				ret = 0;
				break;
			}

			sleep_ms = deadline - now;
			if (sleep_ms > TICK_MS)
				sleep_ms = TICK_MS;

			CURRENT_TASK()->timeout = now + sleep_ms;
		} else {
			CURRENT_TASK()->timeout = time_now_ms() + TICK_MS;
		}

		task_sched();
		CURRENT_TASK()->timeout = 0;

		/* Return EINTR if an unmasked signal arrived. */
		{
			task_struct *cur = CURRENT_TASK();
			if (cur->signal->sig_pending & ~cur->signal->sig_mask)
				return -EINTR;
		}

	} while (1);

	return ret;
}
