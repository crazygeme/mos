#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <hw/time.h>
#include <macro.h>
#include <unistd.h>
#include <errno.h>

typedef struct _pipe_inode {
	cy_buf *buf;
	int readonly;
} pipe_inode;

static int pipe_release(file *fp)
{
	pipe_inode *n = fp->f_inode->i_private;
	if (!n->readonly)
		cyb_writer_close(n->buf);
	else
		cyb_reader_close(n->buf);
	free(n);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static ssize_t pipe_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	pipe_inode *n = fp->f_inode->i_private;

	if (!n->readonly)
		return -1;

	int ret = cyb_getbuf(n->buf, buf, (int)len, 1, 1);
	if (ret < 0)
		return -EINTR;
	return (ssize_t)ret;
}

static ssize_t pipe_write(file *fp, const void *buf, size_t len, loff_t *pos)
{
	pipe_inode *n = fp->f_inode->i_private;
	if (n->readonly)
		return 0;

	int ret = cyb_putbuf(n->buf, (unsigned char *)buf, len, 1, 1);
	return (ssize_t)ret;
}

static int pipe_poll(file *fp, unsigned type)
{
	pipe_inode *n = fp->f_inode->i_private;
	if (type == FS_POLL_EXCEPT)
		return -1;
	if (type == FS_POLL_READ)
		return cyb_isempty(n->buf) ? -1 : 0;
	if (type == FS_POLL_WRITE)
		return cyb_isfull(n->buf) ? -1 : 0;
	return -1;
}

static void pipe_read_poll_wait(file *fp, task_struct *task)
{
	pipe_inode *n = fp->f_inode->i_private;
	cyb_set_poll_read(n->buf, task);
}

static void pipe_read_poll_wait_remove(file *fp, task_struct *task)
{
	pipe_inode *n = fp->f_inode->i_private;
	(void)task;
	cyb_clear_poll_read(n->buf);
}

static void pipe_write_poll_wait(file *fp, task_struct *task)
{
	pipe_inode *n = fp->f_inode->i_private;
	cyb_set_poll_write(n->buf, task);
}

static void pipe_write_poll_wait_remove(file *fp, task_struct *task)
{
	pipe_inode *n = fp->f_inode->i_private;
	(void)task;
	cyb_clear_poll_write(n->buf);
}

static loff_t pipe_llseek(file *fp, loff_t offset, int whence)
{
	/* pipes are not seekable */
	return -ESPIPE;
}

static int pipe_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_mode = node->i_mode;
	s->st_size = 0;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_dev = 0;
	s->st_gid = 0;
	s->st_ino = 0;

	s->st_uid = 0;
	return 0;
}

static const inode_operations pipe_iops = {
	.getattr = pipe_getattr,
};

static const file_operations pipe_read_fops = {
	.release = pipe_release,
	.read = pipe_read,
	.is_ready = pipe_poll,
	.poll_wait = pipe_read_poll_wait,
	.poll_wait_remove = pipe_read_poll_wait_remove,
	.llseek = pipe_llseek,
};

static const file_operations pipe_write_fops = {
	.release = pipe_release,
	.write = pipe_write,
	.is_ready = pipe_poll,
	.poll_wait = pipe_write_poll_wait,
	.poll_wait_remove = pipe_write_poll_wait_remove,
	.llseek = pipe_llseek,
};

int pipe_open(file **pipes)
{
	cy_buf *buf =
		cyb_create(16); /* 16 pages = 64 KB, matching Linux default */

	pipe_inode *rn = zalloc(sizeof(*rn));
	rn->buf = buf;
	rn->readonly = 1;

	inode *ri = zalloc(sizeof(*ri));
	ri->i_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	ri->i_op = &pipe_iops;
	ri->i_private = rn;

	file *fp_read = zalloc(sizeof(*fp_read));
	fp_read->f_inode = ri;
	fp_read->f_count = 1;
	fp_read->f_fop = &pipe_read_fops;

	pipe_inode *wn = zalloc(sizeof(*wn));
	wn->buf = buf;
	wn->readonly = 0;

	inode *wi = zalloc(sizeof(*wi));
	wi->i_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	wi->i_op = &pipe_iops;
	wi->i_private = wn;

	file *fp_write = zalloc(sizeof(*fp_write));
	fp_write->f_fop = &pipe_write_fops;
	fp_write->f_inode = wi;
	fp_write->f_count = 1;

	pipes[0] = fp_read;
	pipes[1] = fp_write;
	return 0;
}
