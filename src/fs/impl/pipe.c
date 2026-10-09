#include <fs/fs.h>
#include <fs/pipe.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <device/time.h>
#include <macro.h>
#include <ps/ps.h>
#include <unistd.h>
#include <errno.h>
#include <lib/command.h>

typedef struct _pipe_inode {
	cy_buf *buf;
	int mode;
	unsigned uid, gid;
} pipe_inode;

static int pipe_release(file *fp)
{
	pipe_inode *n = fp->f_inode->i_private;
	if (n->mode != O_RDONLY)
		cyb_writer_close(n->buf);
	if (n->mode != O_WRONLY)
		cyb_reader_close(n->buf);
	free(n);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static ssize_t pipe_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	pipe_inode *n = fp->f_inode->i_private;
	int blocking, nonblock;

	if (n->mode == O_WRONLY)
		return -EBADF;
	if (!len)
		return 0;

	nonblock = (fp->f_flag & O_NONBLOCK) != 0;
	blocking = nonblock ? 0 : 1;
	int ret = cyb_getbuf(n->buf, buf, (int)len, blocking, 1);
	if (ret < 0)
		return -EINTR;
	if (nonblock && ret == 0 && cyb_writer_count(n->buf) > 0)
		return -EAGAIN;
	return (ssize_t)ret;
}

static ssize_t pipe_write(file *fp, const void *buf, size_t len, loff_t *pos)
{
	pipe_inode *n = fp->f_inode->i_private;
	task_struct *cur = CURRENT_TASK();
	int nonblock;
	int ret;
	if (n->mode == O_RDONLY)
		return -EBADF;
	if (!len)
		return 0;
	nonblock = (fp->f_flag & O_NONBLOCK) != 0;
	if (cyb_reader_count(n->buf) == 0)
		ret = -EPIPE;
	else
		ret = cyb_putbuf(n->buf, (unsigned char *)buf, len, !nonblock,
				 1);
	if (nonblock && ret == 0 && cyb_reader_count(n->buf) > 0)
		ret = -EAGAIN;

	if (ret == -EPIPE && cur && cur->type == ps_user)
		cur->signal->sig_pending |= (1UL << (SIGPIPE - 1));
	return (ssize_t)ret;
}

static unsigned pipe_poll_common(pipe_inode *n, unsigned events, poll_table *pt)
{
	unsigned ready = 0;
	/* An empty synthetic inotify queue must be reported as not-ready while
	 * its writer remains open; this lets nonblocking readers return EAGAIN. */
	if ((events & FS_POLL_READ) && !cyb_isempty(n->buf))
		ready |= FS_POLL_READ;
	if ((events & FS_POLL_HUP) && cyb_writer_count(n->buf) == 0)
		ready |= FS_POLL_HUP;
	if ((events & FS_POLL_ERR) && cyb_reader_count(n->buf) == 0)
		ready |= FS_POLL_ERR;
	if ((events & FS_POLL_WRITE) && cyb_reader_count(n->buf) > 0 &&
	    !cyb_isfull(n->buf))
		ready |= FS_POLL_WRITE;
	if (pt) {
		if (events & (FS_POLL_READ | FS_POLL_HUP))
			cyb_poll_read(n->buf, pt);
		if (events & FS_POLL_WRITE)
			cyb_poll_write(n->buf, pt);
	}
	return ready;
}

static unsigned pipe_read_poll(file *fp, unsigned events, poll_table *pt)
{
	pipe_inode *n = fp->f_inode->i_private;
	return pipe_poll_common(n, events & (FS_POLL_READ | FS_POLL_HUP), pt);
}

static unsigned pipe_write_poll(file *fp, unsigned events, poll_table *pt)
{
	pipe_inode *n = fp->f_inode->i_private;
	return pipe_poll_common(n, events & (FS_POLL_WRITE | FS_POLL_ERR), pt);
}

static unsigned pipe_rw_poll(file *fp, unsigned events, poll_table *pt)
{
	return pipe_poll_common(fp->f_inode->i_private, events, pt);
}

static loff_t pipe_llseek(file *fp, loff_t offset, int whence)
{
	/* pipes are not seekable */
	return -ESPIPE;
}

static int pipe_ioctl_fionread(void *context __attribute__((unused)),
			       unsigned cmd __attribute__((unused)),
			       void *arg __attribute__((unused)))
{
	file *fp = context;
	pipe_inode *n = fp->f_inode->i_private;
	*(int *)arg = cyb_get_buf_len(n->buf);
	return 0;
}

static int pipe_ioctl_fionbio(void *context, unsigned cmd, void *arg)
{
	file *fp = context;
	(void)cmd;
	if (!arg)
		return -EFAULT;
	if (*(int *)arg)
		__sync_fetch_and_or(&fp->f_flag, O_NONBLOCK);
	else
		__sync_fetch_and_and(&fp->f_flag, ~O_NONBLOCK);
	return 0;
}

static const command_operation pipe_commands[256] = {
	[FIONREAD & 255] = { FIONREAD, pipe_ioctl_fionread },
	[FIONBIO & 255] = { FIONBIO, pipe_ioctl_fionbio },
};

static const command_operation *const pipe_command_groups[256] = {
	[(FIONREAD >> 8) & 255] = pipe_commands,
};

static int pipe_ioctl(file *fp, unsigned cmd, void *arg)
{
	return command_dispatch(pipe_command_groups, fp, cmd, arg, -ENOTTY);
}

static int pipe_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;

	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_mode = node->i_mode;
	s->st_size = 0;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_dev = 0;
	pipe_inode *n = node->i_private;
	s->st_gid = n->gid;
	s->st_ino = 0;

	s->st_uid = n->uid;
	return 0;
}

static const file_operations pipe_read_fops = {
	.release = pipe_release,
	.reopen = pipe_reopen,
	.getattr = pipe_getattr,
	.read = pipe_read,
	.poll = pipe_read_poll,
	.llseek = pipe_llseek,
	.ioctl = pipe_ioctl,
};

static const file_operations pipe_write_fops = {
	.release = pipe_release,
	.reopen = pipe_reopen,
	.getattr = pipe_getattr,
	.write = pipe_write,
	.poll = pipe_write_poll,
	.llseek = pipe_llseek,
	.ioctl = pipe_ioctl,
};

static const file_operations pipe_rw_fops = {
	.release = pipe_release,
	.reopen = pipe_reopen,
	.getattr = pipe_getattr,
	.read = pipe_read,
	.write = pipe_write,
	.poll = pipe_rw_poll,
	.llseek = pipe_llseek,
	.ioctl = pipe_ioctl,
};

/* Reopening an anonymous pipe creates independent flags and endpoint counts. */
file *pipe_reopen(file *original, int flags)
{
	pipe_inode *source, *node;
	inode *in;
	file *fp;
	int mode = flags & O_ACCMODE;
	if (!original || (original->f_fop != &pipe_read_fops &&
			  original->f_fop != &pipe_write_fops &&
			  original->f_fop != &pipe_rw_fops))
		return NULL;
	if (mode != O_RDONLY && mode != O_WRONLY && mode != O_RDWR)
		return NULL;
	node = zalloc(sizeof(*node));
	in = zalloc(sizeof(*in));
	fp = zalloc(sizeof(*fp));
	if (!node || !in || !fp) {
		free(node);
		free(in);
		free(fp);
		return NULL;
	}
	source = original->f_inode->i_private;
	node->buf = source->buf;
	node->mode = mode;
	node->uid = source->uid;
	node->gid = source->gid;
	if (mode != O_WRONLY)
		cyb_reader_open(node->buf);
	if (mode != O_RDONLY)
		cyb_writer_open(node->buf);
	in->i_mode = original->f_inode->i_mode;
	in->i_private = node;
	fp->f_inode = in;
	fp->f_count = 1;
	fp->f_mode = mode;
	fp->f_flag = flags;
	fp->f_fop = mode == O_RDONLY ? &pipe_read_fops :
		    mode == O_WRONLY ? &pipe_write_fops :
				       &pipe_rw_fops;
	return fp;
}

int pipe_open(file **pipes)
{
	cy_buf *buf =
		cyb_create(16); /* 16 pages = 64 KB, matching Linux default */

	pipe_inode *rn = zalloc(sizeof(*rn));
	rn->buf = buf;
	rn->mode = O_RDONLY;
	rn->uid = CURRENT_TASK()->user->euid;
	rn->gid = CURRENT_TASK()->user->egid;

	inode *ri = zalloc(sizeof(*ri));
	ri->i_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	ri->i_private = rn;

	file *fp_read = zalloc(sizeof(*fp_read));
	fp_read->f_inode = ri;
	fp_read->f_count = 1;
	fp_read->f_fop = &pipe_read_fops;
	fp_read->f_mode = O_RDONLY;
	fp_read->f_flag = O_RDONLY;

	pipe_inode *wn = zalloc(sizeof(*wn));
	wn->buf = buf;
	wn->mode = O_WRONLY;
	wn->uid = rn->uid;
	wn->gid = rn->gid;

	inode *wi = zalloc(sizeof(*wi));
	wi->i_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	wi->i_private = wn;

	file *fp_write = zalloc(sizeof(*fp_write));
	fp_write->f_fop = &pipe_write_fops;
	fp_write->f_inode = wi;
	fp_write->f_count = 1;
	fp_write->f_mode = O_WRONLY;
	fp_write->f_flag = O_WRONLY;

	pipes[0] = fp_read;
	pipes[1] = fp_write;
	return 0;
}
