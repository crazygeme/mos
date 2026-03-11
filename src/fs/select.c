#include <lib/klib.h>
#include <lib/rbtree.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <fs/select.h>
#include <int/int.h>
#include <ps/ps.h>
#include <elf/exec.h>
#include <errno.h>
#include <config.h>

unsigned select_loop_times = 0;
/*
 * do_select - block until at least one watched fd is ready, a timeout
 * expires, or (if timeout == NULL) forever.
 *
 * To avoid a busy loop, the caller sleeps for one timer tick between
 * successive polls using task->timeout, the same mechanism used by
 * msleep().  The scheduler skips tasks whose timeout has not yet
 * expired, so the task does not consume CPU while waiting.
 *
 * For a timed wait the sleep is capped to min(remaining, one_tick) so
 * we never overshoot the caller's deadline.
 */

#define TICK_MS (1000 / HZ) /* milliseconds per timer tick */

#define ALLOC_SRC(src, dst)                       \
	if (dst) {                                \
		src = calloc(1, sizeof(fd_set));  \
		memcpy(src, dst, sizeof(fd_set)); \
		FD_ZERO(dst);                     \
	}

#define CHECK_FDS(src, dst, type)                      \
	for (i = 0; src && (i < nfds); i++) {          \
		if (FD_ISSET(i, src)) {                \
			if (fs_select(i, type) == 0) { \
				FD_SET(i, dst);        \
				has_set++;             \
				if (ret < has_set)     \
					ret = has_set; \
			}                              \
		}                                      \
	}

int do_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	      const struct timespec *timeout, void *sigmask)
{
	fd_set *reads, *writes, *excepts;
	int ret = 0;
	int i = 0;
	int infinit_wait = 0;
	int just_test = 0;
	unsigned wait_ms = 0;
	unsigned deadline = 0;
	reads = writes = excepts = NULL;

	ALLOC_SRC(reads, readfds);
	ALLOC_SRC(writes, writefds);
	ALLOC_SRC(excepts, exceptfds);

	if (timeout) {
		if (timeout->tv_sec == 0 && timeout->tv_nsec == 0) {
			just_test = 1;
		} else {
			wait_ms = timeout->tv_sec * 1000 +
				  timeout->tv_nsec / 1000000;
			deadline = time_now_ms() + wait_ms;
		}
	} else {
		infinit_wait = 1;
	}

	do {
		int has_set = 0;
		select_loop_times++;

		CHECK_FDS(reads, readfds, FS_POLL_READ);
		CHECK_FDS(writes, writefds, FS_POLL_WRITE);
		CHECK_FDS(excepts, exceptfds, FS_POLL_EXCEPT);

		if (ret > 0)
			break;

		if (just_test) {
			ret = 0;
			break;
		}

		if (!infinit_wait) {
			unsigned now = time_now_ms();
			unsigned sleep_ms;

			if (now >= deadline) {
				ret = -ETIMEDOUT;
				break;
			}

			/* Sleep for one tick or until the deadline. */
			sleep_ms = deadline - now;
			if (sleep_ms > TICK_MS)
				sleep_ms = TICK_MS;

			CURRENT_TASK()->timeout = now + sleep_ms;
		} else {
			/* Infinite wait: sleep one tick then re-poll. */
			CURRENT_TASK()->timeout = time_now_ms() + TICK_MS;
		}

		task_sched();
		CURRENT_TASK()->timeout = 0;

	} while (1);

	if (reads)
		free(reads);
	if (writes)
		free(writes);
	if (excepts)
		free(excepts);

	return ret;
}
