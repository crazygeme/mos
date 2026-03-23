/*
 * src/dev/initctl.c - /dev/initctl named FIFO
 *
 * Implements a persistent named pipe shared across all opens of /dev/initctl.
 * Writers push init requests; the init process reads them.
 *
 * FIFO semantics:
 *   - open(O_RDONLY) → read end; open(O_WRONLY) → write end.
 *   - Reader sees EOF (read returns 0) when all writer ends have been closed.
 *   - Writer gets EPIPE when there are no reader ends open.
 *   - The underlying cyclebuf persists as long as the device is registered;
 *     it is not freed when all FDs are closed.
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <ps/ps.h>
#include <hw/time.h>
#include <macro.h>
#include <dev/dev.h>
#include <fs/fcntl.h>
#include <unistd.h>
#include <errno.h>

/* Per-open context stored in f_inode->i_private. */
typedef struct {
	cy_buf *buf;
	int is_writer; /* 1 → write end, 0 → read end */
} initctl_ctx;

static ssize_t initctl_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	initctl_ctx *ctx = fp->f_inode->i_private;

	/* FIFO read: block until data arrives or all writers have closed. */
	return cyb_getbuf(ctx->buf, (unsigned char *)buf, (unsigned)len, 1);
}

static ssize_t initctl_write(file *fp, const void *buf, size_t len, loff_t *pos)
{
	initctl_ctx *ctx = fp->f_inode->i_private;

	if (cyb_reader_count(ctx->buf) == 0)
		return -EPIPE;

	return (ssize_t)cyb_putbuf(ctx->buf, (unsigned char *)buf,
				   (unsigned)len);
}

static int initctl_poll(file *fp, unsigned type)
{
	initctl_ctx *ctx = fp->f_inode->i_private;

	if (type == FS_POLL_EXCEPT)
		return -1;
	if (type == FS_POLL_READ)
		return cyb_isempty(ctx->buf) ? -1 : 0;
	if (type == FS_POLL_WRITE)
		return cyb_isfull(ctx->buf) ? -1 : 0;
	return -1;
}

static loff_t initctl_llseek(file *fp, loff_t offset, int whence)
{
	/* named pipes are not seekable */
	return 0;
}

static int initctl_release(file *fp)
{
	initctl_ctx *ctx = fp->f_inode->i_private;

	if (ctx->is_writer)
		cyb_writer_close(ctx->buf);
	else
		cyb_reader_close(ctx->buf);

	free(ctx);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int initctl_getattr(inode *node, struct stat *s)
{
	s->st_atime = time_now_ms();
	s->st_mode = node->i_mode;
	s->st_size = 0;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = time_now_ms();
	s->st_dev = 0;
	s->st_rdev = 0;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	return 0;
}

static const inode_operations initctl_iops = {
	.getattr = initctl_getattr,
};

static const file_operations initctl_read_fops = {
	.release = initctl_release,
	.read = initctl_read,
	.poll = initctl_poll,
	.llseek = initctl_llseek,
};

static const file_operations initctl_write_fops = {
	.release = initctl_release,
	.write = initctl_write,
	.poll = initctl_poll,
	.llseek = initctl_llseek,
};

/*
 * initctl_open_root - called on each open() of /dev/initctl.
 *
 * Creates a per-open file descriptor backed by the shared cyclebuf.
 * The open mode (flag) determines whether this is the read or write end:
 *   O_RDONLY → read end  (cyb_reader_open, read fops)
 *   O_WRONLY → write end (cyb_writer_open, write fops)
 *   O_RDWR   → write end (treat as write for simplicity)
 */
static file *initctl_open_root(super_block *sb, int flag)
{
	cy_buf *buf = sb->s_fs_info;
	int is_writer = ((flag & O_ACCMODE) != O_RDONLY);

	if (is_writer)
		cyb_writer_open(buf);
	else
		cyb_reader_open(buf);

	initctl_ctx *ctx = zalloc(sizeof(*ctx));
	ctx->buf = buf;
	ctx->is_writer = is_writer;

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	node->i_op = &initctl_iops;
	node->i_private = ctx;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = is_writer ? &initctl_write_fops : &initctl_read_fops;
	return fp;
}

static void initctl_release_super(super_block *sb)
{
	/* Release the device's reference on the cyclebuf. */
	cyb_destroy(sb->s_fs_info);
	free(sb);
}

static super_operations initctl_sops = {
	.open_root = initctl_open_root,
	.release = initctl_release_super,
};

static void initctl_dev_register(super_block *dev_sb)
{
	super_block *sb = sget(&initctl_sops);
	/* cyb_create_named: starts with reader_count = writer_count = 0;
	 * the device holds one reference so the buffer persists across
	 * open/close cycles. */
	sb->s_fs_info = cyb_create_named(1);
	vfs_mount(dev_sb, "/initctl", sb);
}

DEV_INIT(initctl_dev_register);
