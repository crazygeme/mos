#include <fs/fs.h>
#include <fs/mount.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <hw/time.h>
#include <macro.h>
#include <unistd.h>

typedef struct _pipe_inode {
	cy_buf *buf;
	int readonly;
} pipe_inode;

static int pipe_release(inode *node, file *fp)
{
	pipe_inode *n = node->i_private;
	if (!n->readonly)
		cyb_writer_close(n->buf);
	else
		cyb_reader_close(n->buf);
	free(n);
	free(node);
	return 0;
}

static ssize_t pipe_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	pipe_inode *n = fp->f_inode->i_private;
	unsigned char *tmp = buf;
	unsigned char c;
	int i = 0;

	if (!n->readonly)
		return -1;

	if (cyb_writer_count(n->buf) == 0 && cyb_isempty(n->buf))
		return 0;

	while (i < (int)len) {
		c = cyb_getc(n->buf);
		if (c == EOF)
			break;
		*tmp++ = c;
		i++;
	}
	return (ssize_t)i;
}

static ssize_t pipe_write(file *fp, const void *buf, size_t len, loff_t *pos)
{
	pipe_inode *n = fp->f_inode->i_private;
	unsigned char *tmp = (unsigned char *)buf;

	if (n->readonly)
		return 0;

	cyb_putbuf(n->buf, tmp, len);
	return (ssize_t)len;
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

static loff_t pipe_llseek(file *fp, loff_t offset, int whence)
{
	/* pipes are not seekable */
	return 0;
}

static int pipe_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	s->st_size = 0;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	return 0;
}

static const inode_operations pipe_iops = {
	.getattr = pipe_getattr,
};

static const file_operations pipe_read_fops = {
	.release = pipe_release,
	.read = pipe_read,
	.poll = pipe_poll,
	.llseek = pipe_llseek,
};

static const file_operations pipe_write_fops = {
	.release = pipe_release,
	.write = pipe_write,
	.poll = pipe_poll,
	.llseek = pipe_llseek,
};

int pipe_open(file **pipes)
{
	cy_buf *buf = cyb_create();

	pipe_inode *rn = calloc(1, sizeof(*rn));
	rn->buf = buf;
	rn->readonly = 1;

	inode *ri = calloc(1, sizeof(*ri));
	ri->i_mode = S_IFIFO;
	ri->i_op = &pipe_iops;
	ri->i_fop = &pipe_read_fops;
	ri->i_private = rn;

	file *fp_read = calloc(1, sizeof(*fp_read));
	fp_read->f_inode = ri;
	fp_read->f_op = &pipe_read_fops;
	fp_read->f_count = 1;

	pipe_inode *wn = calloc(1, sizeof(*wn));
	wn->buf = buf;
	wn->readonly = 0;

	inode *wi = calloc(1, sizeof(*wi));
	wi->i_mode = S_IFIFO;
	wi->i_op = &pipe_iops;
	wi->i_fop = &pipe_write_fops;
	wi->i_private = wn;

	file *fp_write = calloc(1, sizeof(*fp_write));
	fp_write->f_inode = wi;
	fp_write->f_op = &pipe_write_fops;
	fp_write->f_count = 1;

	pipes[0] = fp_read;
	pipes[1] = fp_write;
	return 0;
}
