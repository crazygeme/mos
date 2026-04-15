#include <lib/klib.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <fs/poll.h>
#include <fs/select.h>
#include <ps/ps.h>
#include <hw/time.h>
#include <config.h>
#include <errno.h>

/* ---- select(2) implementation ---- */

struct select_ctx {
	int nfds;
	size_t set_bytes;
	/* Snapshots of the caller's input sets (never modified after setup). */
	fd_set *reads, *writes, *excepts;
	/* Pointers to the caller's output sets (zeroed and filled each check). */
	fd_set *readfds, *writefds, *exceptfds;
	poll_table wait;
};

static int select_ctx_check(void *arg)
{
	struct select_ctx *ctx = arg;
	int i, ready = 0;

	if (ctx->readfds)
		memset(ctx->readfds, 0, ctx->set_bytes);
	if (ctx->writefds)
		memset(ctx->writefds, 0, ctx->set_bytes);
	if (ctx->exceptfds)
		memset(ctx->exceptfds, 0, ctx->set_bytes);

	for (i = 0; i < ctx->nfds; i++) {
		unsigned want = 0;
		unsigned fd_ready = 0;
		if (ctx->reads && FD_ISSET(i, ctx->reads))
			want |= FS_POLL_READ;
		if (ctx->writes && FD_ISSET(i, ctx->writes))
			want |= FS_POLL_WRITE;
		if (ctx->excepts && FD_ISSET(i, ctx->excepts))
			want |= FS_POLL_EXCEPT;
		if (want & FS_POLL_READ)
			want |= FS_POLL_HUP;
		if (!want)
			continue;
		if (i >= MAX_FD || CURRENT_TASK()->fds[i] == NULL)
			return -EBADF;
		unsigned ready_mask = fs_fd_poll(i, want, NULL);
		if ((want & FS_POLL_READ) &&
		    (ready_mask & (FS_POLL_READ | FS_POLL_HUP))) {
			FD_SET(i, ctx->readfds);
			fd_ready = 1;
		}
		if ((want & FS_POLL_WRITE) && (ready_mask & FS_POLL_WRITE)) {
			FD_SET(i, ctx->writefds);
			fd_ready = 1;
		}
		if ((want & FS_POLL_EXCEPT) && (ready_mask & FS_POLL_EXCEPT)) {
			FD_SET(i, ctx->exceptfds);
			fd_ready = 1;
		}
		ready += fd_ready;
	}
	return ready;
}

static int select_ctx_reg(void *arg)
{
	struct select_ctx *ctx = arg;
	int i;
	poll_table *pt = &ctx->wait;

	for (i = 0; i < ctx->nfds; i++) {
		unsigned want = 0;
		if (ctx->reads && FD_ISSET(i, ctx->reads))
			want |= FS_POLL_READ;
		if (ctx->writes && FD_ISSET(i, ctx->writes))
			want |= FS_POLL_WRITE;
		if (ctx->excepts && FD_ISSET(i, ctx->excepts))
			want |= FS_POLL_EXCEPT;
		if (want & FS_POLL_READ)
			want |= FS_POLL_HUP;
		if (!want)
			continue;
		if (i >= MAX_FD || CURRENT_TASK()->fds[i] == NULL)
			return -EBADF;
		fs_fd_poll(i, want, pt);
	}
	return pt->unsupported;
}

static void select_ctx_dereg(void *arg)
{
	struct select_ctx *ctx = arg;
	poll_table_cleanup(&ctx->wait);
}

static const struct poll_ops select_fops = {
	.check = select_ctx_check,
	.reg = select_ctx_reg,
	.dereg = select_ctx_dereg,
};

int do_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	      const struct timeval *timeout, void *sigmask)
{
	int ret;
	int just_test = 0, infinite = 0;
	unsigned deadline = 0;
	size_t set_bytes;
	task_struct *cur = CURRENT_TASK();
	sigset_t saved_mask = cur->signal->sig_mask;
	struct select_ctx ctx;
	poll_table_entry *entries = NULL;

	if (nfds < 0 || nfds > FD_SETSIZE)
		return -EINVAL;

	set_bytes = (((unsigned)(nfds ? nfds : 1) + NFDBITS - 1) / NFDBITS) *
		    sizeof(fd_mask);

	memset(&ctx, 0, sizeof(ctx));
	ctx.nfds = nfds;
	ctx.set_bytes = set_bytes;
	if (nfds > 0) {
		entries = zalloc(sizeof(*entries) * nfds * 2);
		poll_table_init(&ctx.wait, cur, entries, nfds * 2);
	} else {
		poll_table_init(&ctx.wait, cur, NULL, 0);
	}

	if (sigmask)
		cur->signal->sig_mask = *(sigset_t *)sigmask;

	/* Snapshot the input sets; output sets are filled on each check. */
	if (readfds) {
		ctx.reads = zalloc(set_bytes);
		memcpy(ctx.reads, readfds, set_bytes);
		ctx.readfds = readfds;
		memset(ctx.readfds, 0, set_bytes);
	}
	if (writefds) {
		ctx.writes = zalloc(set_bytes);
		memcpy(ctx.writes, writefds, set_bytes);
		ctx.writefds = writefds;
		memset(ctx.writefds, 0, set_bytes);
	}
	if (exceptfds) {
		ctx.excepts = zalloc(set_bytes);
		memcpy(ctx.excepts, exceptfds, set_bytes);
		ctx.exceptfds = exceptfds;
		memset(ctx.exceptfds, 0, set_bytes);
	}

	if (timeout) {
		if (timeout->tv_sec == 0 && timeout->tv_usec == 0) {
			just_test = 1;
		} else {
			deadline = time_now_ms() + timeout->tv_sec * 1000 +
				   timeout->tv_usec / 1000;
		}
	} else {
		infinite = 1;
	}

	ret = poll_wait_loop(&select_fops, &ctx, just_test, infinite, deadline);

	if (sigmask)
		cur->signal->sig_mask = saved_mask;

	if (ctx.reads)
		free(ctx.reads);
	if (ctx.writes)
		free(ctx.writes);
	if (ctx.excepts)
		free(ctx.excepts);
	if (entries)
		free(entries);

	return ret;
}
