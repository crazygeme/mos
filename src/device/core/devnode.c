#include <device/chardev.h>
#include <device/blockdev.h>
/*
 * src/device/core/devnode.c - device node created by mknod(2).
 *
 * Each successful mknod() allocates one devnode super_block and mounts it
 * into the VFS tree at the requested path.  Opening the path returns a
 * minimal file whose getattr reports the correct mode/rdev.
 *
 * S_IFIFO  : a cy_buf shared across all opens; reader/writer counts are
 *            managed by cyb_reader_open/cyb_writer_open so that EOF
 *            propagates correctly when all writers close.
 * S_IFCHR / S_IFBLK: registered major/minor ranges dispatch to device
 *                   callbacks; unregistered nodes return -ENXIO for I/O.
 * S_IFSOCK: metadata is available; I/O returns -ENXIO.
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <fs/poll.h>
#include <lib/klib.h>
#include <lib/cyclebuf.h>
#include <ps/ps.h>
#include <device/devnode.h>
#include <errno.h>
#include <macro.h>

/* ------------------------------------------------------------------ *
 * Character / block device dispatch table                             *
 * ------------------------------------------------------------------ */

static file *devnode_reopen(file *fp, int flags);
static int fifonode_open_ready(file *fp);
static const file_operations devnode_fops;
static const file_operations fifonode_reader_fops;
static const file_operations fifonode_writer_fops;

static file *devnode_open_stub(super_block *sb, unsigned mode)
{
	file *fp;
	inode *node;

	node = zalloc(sizeof(*node));
	node->i_mode = mode;
	node->i_private =
		sb->s_fs_info; /* owned by super_block; not freed here */

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &devnode_fops;
	return fp;
}

typedef struct {
	unsigned mode; /* full mode bits, e.g. S_IFCHR | 0666 */
	unsigned rdev; /* encoded major/minor (for char/block) */
	unsigned uid, gid;
	cy_buf *fifo; /* non-NULL for S_IFIFO */
} devnode_info;

/* ------------------------------------------------------------------ *
 * Char / block / socket: stat works; I/O returns ENXIO               *
 * ------------------------------------------------------------------ */

static int devnode_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	devnode_info *dn = node->i_private;

	memset(s, 0, sizeof(*s));
	s->st_mode = dn->mode;
	s->st_rdev = dn->rdev;
	s->st_ino = (uintptr_t)dn;
	s->st_uid = dn->uid;
	s->st_gid = dn->gid;
	s->st_nlink = 1;
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_blksize = PAGE_SIZE;
	return 0;
}

static int devnode_setattr(file *fp, uint32_t mode)
{
	devnode_info *dn = fp->f_inode->i_private;
	dn->mode = (dn->mode & S_IFMT) | (mode & 07777);
	fp->f_inode->i_mode = dn->mode;
	return 0;
}
static int devnode_chown(file *fp, uint32_t uid, uint32_t gid)
{
	devnode_info *dn = fp->f_inode->i_private;
	if (uid != (uint32_t)-1) dn->uid = uid;
	if (gid != (uint32_t)-1) dn->gid = gid;
	return 0;
}

static ssize_t devnode_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	return -ENXIO;
}

static ssize_t devnode_write(file *fp, const void *buf, size_t len, loff_t *pos)
{
	return -ENXIO;
}

static int devnode_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations devnode_fops = {
	.reopen = devnode_reopen,
	.release = devnode_release,
	.getattr = devnode_getattr,
	.setattr = devnode_setattr,
	.chown = devnode_chown,
	.read = devnode_read,
	.write = devnode_write,
};

/* ------------------------------------------------------------------ *
 * FIFO: shared cy_buf between reader and writer opens                 *
 * ------------------------------------------------------------------ */

static int fifonode_getattr(file *fp, struct stat *s)
{
	devnode_info *dn = fp->f_inode->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = dn->mode;
	s->st_uid = dn->uid;
	s->st_gid = dn->gid;
	s->st_nlink = 1;
	s->st_atime = time_wall_sec();
	s->st_ctime = time_wall_sec();
	s->st_mtime = time_wall_sec();
	s->st_blksize = PAGE_SIZE;
	return 0;
}

static ssize_t fifonode_read(file *fp, void *buf, size_t len, loff_t *pos)
{
	cy_buf *b = ((devnode_info *)fp->f_inode->i_private)->fifo;
	int nonblock = (fp->f_flag & O_NONBLOCK) != 0;
	int ret;

	if (!len)
		return 0;
	ret = cyb_getbuf(b, buf, (int)len, !nonblock, 1);
	if (ret < 0)
		return -EINTR;
	if (!ret && nonblock && cyb_writer_count(b))
		return -EAGAIN;
	return ret;
}

static ssize_t fifonode_write(file *fp, const void *buf, size_t len,
			      loff_t *pos)
{
	cy_buf *b = ((devnode_info *)fp->f_inode->i_private)->fifo;
	int nonblock = (fp->f_flag & O_NONBLOCK) != 0;
	int ret;

	if (!len)
		return 0;
	ret = cyb_reader_count(b) ? cyb_putbuf(b, (unsigned char *)buf,
					       (unsigned)len, !nonblock, 1) :
				    -EPIPE;
	if (!ret && nonblock && cyb_reader_count(b))
		ret = -EAGAIN;
	if (ret == -EPIPE && current->life->type == ps_user)
		ps_send_signal(current->life->psid, SIGPIPE);
	return ret;
}

static unsigned fifonode_poll_common(cy_buf *b, unsigned events, poll_table *pt)
{
	unsigned ready = 0;

	if ((events & FS_POLL_READ) && !cyb_isempty(b))
		ready |= FS_POLL_READ;
	if ((events & FS_POLL_HUP) && !cyb_writer_count(b))
		ready |= FS_POLL_HUP;
	if ((events & FS_POLL_ERR) && !cyb_reader_count(b))
		ready |= FS_POLL_ERR;
	if ((events & FS_POLL_WRITE) && cyb_reader_count(b) && !cyb_isfull(b))
		ready |= FS_POLL_WRITE;
	if (pt) {
		if (events & (FS_POLL_READ | FS_POLL_HUP))
			cyb_poll_read(b, pt);
		if (events & (FS_POLL_WRITE | FS_POLL_ERR))
			cyb_poll_write(b, pt);
	}
	return ready;
}

static unsigned fifonode_read_poll(file *fp, unsigned events, poll_table *pt)
{
	cy_buf *b = ((devnode_info *)fp->f_inode->i_private)->fifo;
	return fifonode_poll_common(b, events & (FS_POLL_READ | FS_POLL_HUP),
				    pt);
}

static unsigned fifonode_write_poll(file *fp, unsigned events, poll_table *pt)
{
	cy_buf *b = ((devnode_info *)fp->f_inode->i_private)->fifo;
	return fifonode_poll_common(b, events & (FS_POLL_WRITE | FS_POLL_ERR),
				    pt);
}

static int fifonode_release_reader(file *fp)
{
	cy_buf *b = ((devnode_info *)fp->f_inode->i_private)->fifo;
	cyb_reader_close(b);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int fifonode_release_writer(file *fp)
{
	cy_buf *b = ((devnode_info *)fp->f_inode->i_private)->fifo;
	cyb_writer_close(b);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations fifonode_reader_fops = {
	.reopen = devnode_reopen,
	.open_ready = fifonode_open_ready,
	.release = fifonode_release_reader,
	.getattr = fifonode_getattr,
	.setattr = devnode_setattr,
	.chown = devnode_chown,
	.read = fifonode_read,
	.poll = fifonode_read_poll,
};

static const file_operations fifonode_writer_fops = {
	.reopen = devnode_reopen,
	.open_ready = fifonode_open_ready,
	.release = fifonode_release_writer,
	.getattr = fifonode_getattr,
	.setattr = devnode_setattr,
	.chown = devnode_chown,
	.write = fifonode_write,
	.poll = fifonode_write_poll,
};

typedef struct {
	inode node;
	unsigned peer_sequence;
} fifo_open_inode;
static int fifo_peer_check(void *opaque)
{
	file *fp = opaque;
	cy_buf *b = ((devnode_info *)fp->f_inode->i_private)->fifo;
	int writer = fp->f_mode != O_WRONLY;
	fifo_open_inode *opened =
		container_of(fp->f_inode, fifo_open_inode, node);
	return (writer ? cyb_writer_count(b) : cyb_reader_count(b)) ||
	       cyb_open_sequence(b, writer) != opened->peer_sequence;
}
struct fifo_peer_wait {
	file *fp;
	poll_table table;
};
static int fifo_wait_check(void *opaque)
{
	struct fifo_peer_wait *wait = opaque;
	return fifo_peer_check(wait->fp);
}
static int fifo_wait_register(void *opaque)
{
	struct fifo_peer_wait *wait = opaque;
	cy_buf *b = ((devnode_info *)wait->fp->f_inode->i_private)->fifo;
	if (wait->fp->f_mode == O_WRONLY)
		cyb_poll_write(b, &wait->table);
	else
		cyb_poll_read(b, &wait->table);
	return wait->table.unsupported;
}
static void fifo_wait_deregister(void *opaque)
{
	struct fifo_peer_wait *wait = opaque;
	poll_table_cleanup(&wait->table);
}
static int fifonode_open_ready(file *fp)
{
	if (fp->f_mode == O_RDWR || fifo_peer_check(fp))
		return 0;
	if (fp->f_flag & O_NONBLOCK)
		return fp->f_mode == O_WRONLY ? -ENXIO : 0;
	struct fifo_peer_wait wait = { .fp = fp };
	poll_table_entry entry;
	poll_table_init(&wait.table, current, &entry, 1);
	static const struct poll_ops ops = { .check = fifo_wait_check,
					     .reg = fifo_wait_register,
					     .dereg = fifo_wait_deregister };
	int ret = poll_wait_loop(&ops, &wait, 0, 1, 0);
	poll_table_cleanup(&wait.table);
	return ret < 0 ? ret : 0;
}
static int fifonode_release_rw(file *fp)
{
	cy_buf *b = ((devnode_info *)fp->f_inode->i_private)->fifo;
	cyb_reader_close(b);
	cyb_writer_close(b);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static unsigned fifonode_rw_poll(file *fp, unsigned events, poll_table *pt)
{
	return fifonode_read_poll(fp, events, pt) |
	       fifonode_write_poll(fp, events, pt);
}
static const file_operations fifonode_rw_fops = {
	.reopen = devnode_reopen,
	.open_ready = fifonode_open_ready,
	.release = fifonode_release_rw,
	.getattr = fifonode_getattr,
	.setattr = devnode_setattr,
	.chown = devnode_chown,
	.read = fifonode_read,
	.write = fifonode_write,
	.poll = fifonode_rw_poll,
};

static file *devnode_open_fifo(devnode_info *dn, int flag)
{
	cy_buf *b = dn->fifo;
	fifo_open_inode *opened = zalloc(sizeof(*opened));
	inode *node = &opened->node;
	opened->peer_sequence =
		cyb_open_sequence(b, (flag & O_ACCMODE) != O_WRONLY);
	file *fp = zalloc(sizeof(*fp));

	node->i_mode = dn->mode;
	node->i_private = dn;

	fp->f_inode = node;
	fp->f_count = 1;

	if ((flag & O_ACCMODE) == O_RDWR) {
		cyb_reader_open(b);
		cyb_writer_open(b);
		fp->f_fop = &fifonode_rw_fops;
	} else if ((flag & 3) == O_WRONLY) {
		cyb_writer_open(b);
		fp->f_fop = &fifonode_writer_fops;
	} else {
		cyb_reader_open(b);
		fp->f_fop = &fifonode_reader_fops;
	}

	return fp;
}

static file *devnode_open_node(super_block *sb, devnode_info *dn, int flag)
{
	if (flag & O_PATH)
		return devnode_open_stub(sb, dn->mode);
	int matched = 0;
	file *fp = S_ISCHR(dn->mode) ?
			   chardev_open(sb, dn->rdev, flag, &matched) :
			   blockdev_open_node(sb, dn->rdev, flag, &matched);
	if (!fp && (!matched || (flag & O_PATH)))
		return devnode_open_stub(sb, dn->mode);
	return fp;
}

/* ------------------------------------------------------------------ *
 * super_operations                                                     *
 * ------------------------------------------------------------------ */

static file *devnode_open_fifo_root(super_block *sb, devnode_info *dn, int flag)
{
	return flag & O_PATH ? devnode_open_stub(sb, dn->mode) :
			       devnode_open_fifo(dn, flag);
}

static file *devnode_open_stub_root(super_block *sb, devnode_info *dn, int flag)
{
	return devnode_open_stub(sb, dn->mode);
}

static file *(*const devnode_openers[16])(super_block *, devnode_info *,
					  int) = {
	[S_IFIFO >> 12] = devnode_open_fifo_root,
	[S_IFCHR >> 12] = devnode_open_node,
	[S_IFBLK >> 12] = devnode_open_node,
};

static file *devnode_open_root(super_block *sb, int flag)
{
	devnode_info *dn = sb->s_fs_info;
	if (!(flag & O_PATH)) {
		struct stat st = { .st_mode = dn->mode, .st_uid = dn->uid, .st_gid = dn->gid };
		int access = (flag & O_ACCMODE) == O_RDONLY ? 4 :
			(flag & O_ACCMODE) == O_WRONLY ? 2 : 6;
		int error = fs_check_perm(&st, access);
		if (error) {
			current->execution->fs_open_error = error;
			return NULL;
		}
	}
	file *(*open)(super_block *, devnode_info *, int) =
		devnode_openers[(dn->mode & S_IFMT) >> 12];
	if (!open)
		open = devnode_open_stub_root;
	return open(sb, dn, flag);
}

static file *devnode_reopen(file *fp, int flags)
{
	if (!fp->f_sb)
		return NULL;
	file *next = devnode_open_root(fp->f_sb, flags);
	if (next) {
		vfs_set_file_origin(next, fp->f_sb, "");
		next->f_name = fp->f_name ? strdup(fp->f_name) : NULL;
		next->f_mount_flags = fp->f_mount_flags;
	}
	return next;
}

static void devnode_release_super(super_block *sb)
{
	devnode_info *dn = sb->s_fs_info;
	if (dn->fifo)
		cyb_destroy(dn->fifo);
	free(dn);
	kfree(sb);
}

static const super_operations devnode_sops = {
	.open_root = devnode_open_root,
	.release = devnode_release_super,
};

/*
 * devnode_create - allocate a new devnode super_block.
 *
 * Called by vfs_mknod(); the returned super_block is mounted at the
 * requested path and thereafter owned by the VFS mount tree.
 */
super_block *devnode_create(unsigned mode, unsigned rdev)
{
	devnode_info *dn = zalloc(sizeof(*dn));
	task_struct *cur = CURRENT_TASK();
	super_block *sb;

	dn->mode = mode;
	dn->rdev = rdev;
	if (cur && cur->execution) {
		dn->uid = cur->credentials->euid;
		dn->gid = cur->credentials->egid;
	}
	if (S_ISFIFO(mode))
		dn->fifo = cyb_create_named(0);

	sb = sget(&devnode_sops);
	sb->s_fs_info = dn;
	return sb;
}
