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
		if (ctx->reads && FD_ISSET(i, ctx->reads) &&
		    fs_fd_ready(i, FS_POLL_READ) == 0) {
			FD_SET(i, ctx->readfds);
			ready++;
		}
		if (ctx->writes && FD_ISSET(i, ctx->writes) &&
		    fs_fd_ready(i, FS_POLL_WRITE) == 0) {
			FD_SET(i, ctx->writefds);
			ready++;
		}
		if (ctx->excepts && FD_ISSET(i, ctx->excepts) &&
		    fs_fd_ready(i, FS_POLL_EXCEPT) == 0) {
			FD_SET(i, ctx->exceptfds);
			ready++;
		}
	}
	return ready;
}

static int select_ctx_reg(void *arg)
{
	struct select_ctx *ctx = arg;
	task_struct *cur = CURRENT_TASK();
	int i, has_unsupported = 0;

	for (i = 0; i < ctx->nfds; i++) {
		int any = ((ctx->reads && FD_ISSET(i, ctx->reads)) ||
			   (ctx->writes && FD_ISSET(i, ctx->writes)) ||
			   (ctx->excepts && FD_ISSET(i, ctx->excepts)));
		if (!any)
			continue;
		if (i >= (int)MAX_FD || !cur->fds[i].used)
			continue;
		file *fp = cur->fds[i].fp;
		if (!fp || !fp->f_fop)
			continue;
		if (fp->f_fop->poll_wait)
			fp->f_fop->poll_wait(fp, cur);
		else
			has_unsupported = 1;
	}
	return has_unsupported;
}

static void select_ctx_dereg(void *arg)
{
	struct select_ctx *ctx = arg;
	task_struct *cur = CURRENT_TASK();
	int i;

	for (i = 0; i < ctx->nfds; i++) {
		int any = ((ctx->reads && FD_ISSET(i, ctx->reads)) ||
			   (ctx->writes && FD_ISSET(i, ctx->writes)) ||
			   (ctx->excepts && FD_ISSET(i, ctx->excepts)));
		if (!any)
			continue;
		if (i >= (int)MAX_FD || !cur->fds[i].used)
			continue;
		file *fp = cur->fds[i].fp;
		if (fp && fp->f_fop && fp->f_fop->poll_wait_remove)
			fp->f_fop->poll_wait_remove(fp, cur);
	}
}

static const struct poll_ops select_fops = {
	.check = select_ctx_check,
	.reg = select_ctx_reg,
	.dereg = select_ctx_dereg,
};

int do_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	      const struct timespec *timeout, void *sigmask)
{
	int ret;
	int just_test = 0, infinite = 0;
	unsigned deadline = 0;
	size_t set_bytes;
	task_struct *cur = CURRENT_TASK();
	sigset_t saved_mask = cur->signal->sig_mask;
	struct select_ctx ctx;

	if (nfds < 0 || nfds > FD_SETSIZE)
		return -EINVAL;

	set_bytes = (((unsigned)(nfds ? nfds : 1) + NFDBITS - 1) / NFDBITS) *
		    sizeof(fd_mask);

	memset(&ctx, 0, sizeof(ctx));
	ctx.nfds = nfds;
	ctx.set_bytes = set_bytes;

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
		if (timeout->tv_sec == 0 && timeout->tv_nsec == 0) {
			just_test = 1;
		} else {
			deadline = time_now_ms() + timeout->tv_sec * 1000 +
				   timeout->tv_nsec / 1000000;
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

	return ret;
}
