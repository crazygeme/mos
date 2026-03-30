#ifndef _FS_POLL_H
#define _FS_POLL_H

/* poll event flags */
#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

struct pollfd {
	int fd;
	short events;
	short revents;
};

/*
 * Callback table for poll_wait_loop.  Each implementation supplies three
 * ops that operate on an opaque context pointer:
 *   check  – inspect all fds; return number of ready fds (fills output state)
 *   reg    – register current task on every fd for wakeup;
 *            return 1 if any fd lacks poll_wait support (needs timer fallback)
 *   dereg  – remove the current task's wakeup registration from every fd
 */
struct poll_ops {
	int (*check)(void *ctx);
	int (*reg)(void *ctx);
	void (*dereg)(void *ctx);
};

/*
 * Generic 4-phase wait loop shared by poll and select.
 * Returns number of ready fds, 0 on timeout, or -EINTR.
 */
int poll_wait_loop(const struct poll_ops *ops, void *ctx, int just_test,
		   int infinite, unsigned deadline);

int do_poll(struct pollfd *fds, unsigned nfds, int timeout);

#endif
