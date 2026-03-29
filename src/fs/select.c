#include <lib/klib.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <fs/select.h>
#include <ps/ps.h>
#include <hw/time.h>
#include <errno.h>
#include <config.h>

#define TICK_MS (1000 / HZ)

/*
 * Check readiness of all watched fds and fill the output fd_sets.
 * The output sets are zeroed before filling.  Returns number of ready fds.
 */
static int select_check(int nfds, fd_set *reads, fd_set *writes,
			fd_set *excepts, fd_set *readfds, fd_set *writefds,
			fd_set *exceptfds)
{
	int i, ready = 0;

	if (readfds)
		FD_ZERO(readfds);
	if (writefds)
		FD_ZERO(writefds);
	if (exceptfds)
		FD_ZERO(exceptfds);

	for (i = 0; i < nfds; i++) {
		if (reads && FD_ISSET(i, reads) &&
		    fs_fd_ready(i, FS_POLL_READ) == 0) {
			FD_SET(i, readfds);
			ready++;
		}
		if (writes && FD_ISSET(i, writes) &&
		    fs_fd_ready(i, FS_POLL_WRITE) == 0) {
			FD_SET(i, writefds);
			ready++;
		}
		if (excepts && FD_ISSET(i, excepts) &&
		    fs_fd_ready(i, FS_POLL_EXCEPT) == 0) {
			FD_SET(i, exceptfds);
			ready++;
		}
	}
	return ready;
}

/*
 * Register task on every watched fd that supports poll_wait.
 * Returns 1 if any fd lacks poll_wait support (needs fallback timer).
 */
static int select_reg(int nfds, fd_set *reads, fd_set *writes, fd_set *excepts,
		      task_struct *task)
{
	task_struct *cur = CURRENT_TASK();
	int i, has_unsupported = 0;

	for (i = 0; i < nfds; i++) {
		int any = ((reads && FD_ISSET(i, reads)) ||
			   (writes && FD_ISSET(i, writes)) ||
			   (excepts && FD_ISSET(i, excepts)));
		if (!any)
			continue;
		if (i >= (int)MAX_FD || !cur->fds[i].used)
			continue;
		file *fp = cur->fds[i].fp;
		if (!fp || !fp->f_fop)
			continue;
		if (fp->f_fop->poll_wait)
			fp->f_fop->poll_wait(fp, task);
		else
			has_unsupported = 1;
	}
	return has_unsupported;
}

/* Deregister task from every watched fd. */
static void select_dereg(int nfds, fd_set *reads, fd_set *writes,
			 fd_set *excepts, task_struct *task)
{
	task_struct *cur = CURRENT_TASK();
	int i;

	for (i = 0; i < nfds; i++) {
		int any = ((reads && FD_ISSET(i, reads)) ||
			   (writes && FD_ISSET(i, writes)) ||
			   (excepts && FD_ISSET(i, excepts)));
		if (!any)
			continue;
		if (i >= (int)MAX_FD || !cur->fds[i].used)
			continue;
		file *fp = cur->fds[i].fp;
		if (fp && fp->f_fop && fp->f_fop->poll_wait_remove)
			fp->f_fop->poll_wait_remove(fp, task);
	}
}

int do_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	      const struct timespec *timeout, void *sigmask)
{
	fd_set *reads = NULL, *writes = NULL, *excepts = NULL;
	int ret = 0;
	int just_test = 0, infinite = 0;
	unsigned deadline = 0;
	task_struct *cur = CURRENT_TASK();
	sigset_t saved_mask = cur->signal->sig_mask;

	if (sigmask)
		cur->signal->sig_mask = *(sigset_t *)sigmask;

	/* Snapshot the input sets; the output sets are filled on each check. */
	if (readfds) {
		reads = zalloc(sizeof(fd_set));
		memcpy(reads, readfds, sizeof(fd_set));
	}
	if (writefds) {
		writes = zalloc(sizeof(fd_set));
		memcpy(writes, writefds, sizeof(fd_set));
	}
	if (exceptfds) {
		excepts = zalloc(sizeof(fd_set));
		memcpy(excepts, exceptfds, sizeof(fd_set));
	}

	if (timeout) {
		if (timeout->tv_sec == 0 && timeout->tv_nsec == 0) {
			just_test = 1;
		} else {
			deadline = time_now_ms() + timeout->tv_sec * 1000 +
				   timeout->tv_nsec / 1000000;
		}
	} else {
		infinite = 1;
	}

	for (;;) {
		/* Phase 1: check current readiness */
		ret = select_check(nfds, reads, writes, excepts, readfds,
				   writefds, exceptfds);
		if (ret > 0)
			break;

		if (just_test)
			break;

		if (!infinite && time_now_ms() >= deadline)
			break;

		/* Phase 2: register wakeup on each fd */
		int has_unsupported =
			select_reg(nfds, reads, writes, excepts, cur);

		/* Phase 3: re-check to close the lost-wakeup window */
		ret = select_check(nfds, reads, writes, excepts, readfds,
				   writefds, exceptfds);
		if (ret > 0) {
			select_dereg(nfds, reads, writes, excepts, cur);
			break;
		}

		/* Phase 4: sleep */
		unsigned sleep_ms = 0;
		if (!infinite) {
			unsigned now = time_now_ms();
			if (now >= deadline) {
				select_dereg(nfds, reads, writes, excepts, cur);
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
		select_dereg(nfds, reads, writes, excepts, cur);

		cur = CURRENT_TASK();
		if (cur->signal->sig_pending & ~cur->signal->sig_mask) {
			ret = -EINTR;
			break;
		}
	}

	if (sigmask)
		cur->signal->sig_mask = saved_mask;

	if (reads)
		free(reads);
	if (writes)
		free(writes);
	if (excepts)
		free(excepts);

	return ret;
}
