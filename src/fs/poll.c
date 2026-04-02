#include <lib/klib.h>
#include <fs/fs.h>
#include <fs/poll.h>
#include <ps/ps.h>
#include <hw/time.h>
#include <errno.h>
#include <config.h>

/*
 * Generic 4-phase wait loop used by both poll and select.
 *
 * Phase 1: check readiness immediately.
 * Phase 2: register the current task for wakeup on every watched fd.
 * Phase 3: re-check to close the lost-wakeup window between phases 1 and 2.
 * Phase 4: sleep; after waking, deregister and check for signals.
 */
int poll_wait_loop(const struct poll_ops *ops, void *ctx, int just_test,
		   int infinite, unsigned deadline)
{
	task_struct *cur;
	int ret;

	for (;;) {
		ret = ops->check(ctx);
		if (ret > 0)
			break;

		if (just_test)
			break;

		if (!infinite && time_now_ms() >= deadline)
			break;

		int has_unsupported = ops->reg(ctx);

		ret = ops->check(ctx);
		if (ret > 0) {
			ops->dereg(ctx);
			break;
		}

		unsigned sleep_ms = 0;
		if (!infinite) {
			unsigned now = time_now_ms();
			if (now >= deadline) {
				ops->dereg(ctx);
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
		ops->dereg(ctx);

		cur = CURRENT_TASK();
		if (cur->signal->sig_pending & ~cur->signal->sig_mask) {
			ret = -EINTR;
			break;
		}
	}

	return ret;
}

/* ---- poll(2) implementation ---- */

struct poll_ctx {
	struct pollfd *fds;
	unsigned nfds;
};

static int poll_ctx_check(void *arg)
{
	struct poll_ctx *ctx = arg;
	unsigned i;
	int ready = 0;

	for (i = 0; i < ctx->nfds; i++) {
		short revents = 0;
		int fd = ctx->fds[i].fd;
		short events = ctx->fds[i].events;

		if (fd < 0) {
			ctx->fds[i].revents = 0;
			continue;
		}
		if (events & (POLLIN | POLLPRI))
			if (fs_fd_ready(fd, FS_POLL_READ) == 0)
				revents |= POLLIN;
		if (events & POLLOUT)
			if (fs_fd_ready(fd, FS_POLL_WRITE) == 0)
				revents |= POLLOUT;
		if (fs_fd_ready(fd, FS_POLL_EXCEPT) == 0)
			revents |= POLLERR;
		ctx->fds[i].revents = revents;
		if (revents)
			ready++;
	}
	return ready;
}

static int poll_ctx_reg(void *arg)
{
	struct poll_ctx *ctx = arg;
	task_struct *cur = CURRENT_TASK();
	unsigned i;
	int has_unsupported = 0;

	for (i = 0; i < ctx->nfds; i++) {
		int fd = ctx->fds[i].fd;
		if (fd < 0 || fd >= (int)MAX_FD || !cur->fds[fd].used)
			continue;
		file *fp = cur->fds[fd].fp;
		if (!fp || !fp->f_fop)
			continue;
		if (fp->f_fop->poll_wait)
			fp->f_fop->poll_wait(fp, cur);
		else
			has_unsupported = 1;
	}
	return has_unsupported;
}

static void poll_ctx_dereg(void *arg)
{
	struct poll_ctx *ctx = arg;
	task_struct *cur = CURRENT_TASK();
	unsigned i;

	for (i = 0; i < ctx->nfds; i++) {
		int fd = ctx->fds[i].fd;
		if (fd < 0 || fd >= (int)MAX_FD || !cur->fds[fd].used)
			continue;
		file *fp = cur->fds[fd].fp;
		if (fp && fp->f_fop && fp->f_fop->poll_wait_remove)
			fp->f_fop->poll_wait_remove(fp, cur);
	}
}

static const struct poll_ops poll_fops = {
	.check = poll_ctx_check,
	.reg = poll_ctx_reg,
	.dereg = poll_ctx_dereg,
};

int do_poll(struct pollfd *fds, unsigned nfds, int timeout)
{
	int just_test = (timeout == 0);
	int infinite = (timeout < 0);
	unsigned deadline = 0;
	struct poll_ctx ctx = { fds, nfds };

	if (!fds && nfds > 0)
		return -EFAULT;

	if (!just_test && !infinite)
		deadline = time_now_ms() + (unsigned)timeout;

	return poll_wait_loop(&poll_fops, &ctx, just_test, infinite, deadline);
}
