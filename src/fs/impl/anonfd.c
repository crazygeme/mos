#include <fs/anonfd.h>
#include <fs/fcntl.h>
#include <fs/poll.h>
#include <ps/ps.h>
#include <errno.h>

struct anonfd_inode {
	inode node;
	unsigned uid, gid;
};

file *anonfd_get(int fd)
{
	file *fp = NULL;
	if (fd < 0 || fd >= MAX_FD)
		return NULL;
	mutex_lock(&current->files->lock);
	fp = current->files->fds[fd];
	if (fp)
		fs_get_file(fp);
	mutex_unlock(&current->files->lock);
	return fp;
}

/* State ownership transfers only after successful allocation. */
int anonfd_install(void *state, const file_operations *ops, int flags)
{
	file *fp = zalloc(sizeof(*fp));
	struct anonfd_inode *storage = zalloc(sizeof(*storage));
	inode *node = storage ? &storage->node : NULL;
	if (!fp || !node) {
		free(fp);
		free(node);
		return -ENOMEM;
	}
	node->i_private = state;
	node->i_mode = S_IRUSR | S_IWUSR;
	storage->uid = current->credentials->euid;
	storage->gid = current->credentials->egid;
	fp->f_inode = node;
	fp->f_fop = ops;
	fp->f_count = 1;
	fp->f_mode = O_RDWR;
	fp->f_flag = O_RDWR | (flags & O_NONBLOCK);
	int fd = fs_install_fd(fp, flags);
	if (fd < 0) {
		/* The caller retains state ownership on installation failure. */
		free(node);
		free(fp);
		return fd < -1 ? fd : -EMFILE;
	}
	return fd;
}

struct anonfd_wait {
	file *fp;
	int (*check)(void *);
	void *context;
	poll_table table;
	poll_table_entry entries[4];
};
static int anonfd_check(void *opaque)
{
	struct anonfd_wait *wait = opaque;
	return wait->check(wait->context);
}
static void anonfd_cancel(void *opaque)
{
	struct anonfd_wait *wait = opaque;
	poll_table_cleanup(&wait->table);
	fs_put_file(wait->fp);
}
static int anonfd_register(void *opaque)
{
	struct anonfd_wait *wait = opaque;
	wait->fp->f_fop->poll(wait->fp, FS_POLL_READ, &wait->table);
	current->wait->io_wait = wait;
	current->wait->cancel_io_wait = anonfd_cancel;
	return wait->table.unsupported;
}
static void anonfd_unregister(void *opaque)
{
	struct anonfd_wait *wait = opaque;
	poll_table_cleanup(&wait->table);
	if (current->wait->io_wait == wait) {
		current->wait->io_wait = NULL;
		current->wait->cancel_io_wait = NULL;
	}
}
static const struct poll_ops anonfd_wait_ops = {
	.check = anonfd_check,
	.reg = anonfd_register,
	.dereg = anonfd_unregister,
};
int anonfd_read_wait(file *fp, int (*check)(void *), void *context)
{
	struct anonfd_wait wait = { .fp = fp,
				    .check = check,
				    .context = context };
	fs_get_file(fp);
	poll_table_init(&wait.table, current, wait.entries, 4);
	int ret = poll_wait_loop(&anonfd_wait_ops, &wait,
				 !!(fp->f_flag & O_NONBLOCK), 1, 0);
	fs_put_file(fp);
	return ret ? ret : -EAGAIN;
}
int anonfd_getattr(file *fp, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IRUSR | S_IWUSR;
	st->st_nlink = 1;
	st->st_uid = ((struct anonfd_inode *)fp->f_inode)->uid;
	st->st_gid = ((struct anonfd_inode *)fp->f_inode)->gid;
	return 0;
}
loff_t anonfd_llseek(file *fp, loff_t offset, int whence)
{
	return 0;
}
