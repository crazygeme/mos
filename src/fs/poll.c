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
		if (ret != 0)
			break;

		if (just_test)
			break;

		if (!infinite && time_now_ms() >= deadline)
			break;

		int has_unsupported = ops->reg(ctx);
		if (has_unsupported < 0) {
			ret = has_unsupported;
			break;
		}

		ret = ops->check(ctx);
		if (ret != 0) {
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

		/*
		 * If an fd became ready around the same time as a signal, prefer
		 * reporting readiness over EINTR. This avoids losing a real wakeup
		 * to a concurrent SIGCHLD/SIGALRM race.
		 */
		ret = ops->check(ctx);
		if (ret > 0)
			break;
		if (ret < 0)
			break;

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
	poll_table wait;
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
		if (fd >= MAX_FD || current->fds[fd] == NULL) {
			ctx->fds[i].revents = POLLNVAL;
			ready++;
			continue;
		}
		unsigned want = 0;
		if (events & POLLIN)
			want |= FS_POLL_READ;
		if (events & POLLPRI)
			want |= FS_POLL_EXCEPT;
		if (events & POLLOUT)
			want |= FS_POLL_WRITE;
		want |= FS_POLL_ERR | FS_POLL_HUP;
		unsigned ready_mask = fs_fd_poll(fd, want, NULL);
		if (ready_mask & FS_POLL_READ)
			revents |= POLLIN;
		if (ready_mask & FS_POLL_WRITE)
			revents |= POLLOUT;
		if (ready_mask & FS_POLL_EXCEPT)
			revents |= POLLPRI;
		if (ready_mask & FS_POLL_ERR)
			revents |= POLLERR;
		if (ready_mask & FS_POLL_HUP)
			revents |= POLLHUP;
		ctx->fds[i].revents = revents;
		if (revents)
			ready++;
	}
	return ready;
}

static int poll_ctx_reg(void *arg)
{
	struct poll_ctx *ctx = arg;
	unsigned i;
	poll_table *pt = &ctx->wait;

	for (i = 0; i < ctx->nfds; i++) {
		int fd = ctx->fds[i].fd;
		short events = ctx->fds[i].events;
		unsigned want = 0;
		if (fd < 0)
			continue;
		if (events & POLLIN)
			want |= FS_POLL_READ;
		if (events & POLLPRI)
			want |= FS_POLL_EXCEPT;
		if (events & POLLOUT)
			want |= FS_POLL_WRITE;
		want |= FS_POLL_ERR | FS_POLL_HUP;
		fs_fd_poll(fd, want, pt);
	}
	return pt->unsupported;
}

static void poll_ctx_dereg(void *arg)
{
	struct poll_ctx *ctx = arg;
	poll_table_cleanup(&ctx->wait);
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
	poll_table_entry *entries = NULL;

	if (!fds && nfds > 0)
		return -EFAULT;

	if (nfds > 0) {
		entries = zalloc(sizeof(*entries) * nfds * 2);
		poll_table_init(&ctx.wait, CURRENT_TASK(), entries, nfds * 2);
	} else {
		poll_table_init(&ctx.wait, CURRENT_TASK(), NULL, 0);
	}

	if (!just_test && !infinite)
		deadline = time_now_ms() + (unsigned)timeout;

	int ret =
		poll_wait_loop(&poll_fops, &ctx, just_test, infinite, deadline);
	if (entries)
		free(entries);
	return ret;
}
