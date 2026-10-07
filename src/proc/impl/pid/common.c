/*
 * common.c — shared inode/file constructors and state helpers for /proc/{pid}.
 *
 * Provides:
 *   make_pid_file()    — wrap proc_buf_t as a read-only regular file
 *   make_pid_dir()     — wrap proc_buf_t as a directory file
 *   make_pid_symlink() — build a symlink file with strdup'd target
 *   pid_state_char()   — single-char process state (Linux stat format)
 *   pid_state_name()   — human-readable process state name
 */
#include "proc_pid.h"
#include <macro.h>
#include <ext4.h>

/* ── File operations shared by regular files and directories ─────────── */

typedef struct {
	proc_buf_t *buffer;
	unsigned uid;
	unsigned gid;
	unsigned task_group;
	int task_directory;
} pid_file_data;

static pid_file_data *pid_data_new(proc_buf_t *pb, task_struct *task)
{
	pid_file_data *data = zalloc(sizeof(*data));
	data->buffer = pb;
	data->uid = task->user->euid;
	data->gid = task->user->egid;
	return data;
}

static ssize_t pid_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	pid_file_data *data = fp->f_inode->i_private;
	proc_buf_t *pb = data->buffer;
	loff_t off = *pos;
	ssize_t left = (ssize_t)pb->len - (ssize_t)off;
	ssize_t n = (ssize_t)count < left ? (ssize_t)count : left;

	if (n <= 0)
		return 0;
	memcpy(buf, pb->buf + off, (size_t)n);
	*pos = off + n;
	return n;
}

static loff_t pid_llseek(file *fp, loff_t offset, int whence)
{
	pid_file_data *data = fp->f_inode->i_private;
	proc_buf_t *pb = data->buffer;
	loff_t fsize = (loff_t)pb->len;
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = fp->f_pos + offset;
		break;
	case SEEK_END:
		newpos = fsize + offset;
		break;
	default:
		return -EINVAL;
	}
	if (newpos < 0)
		return -EINVAL;
	fp->f_pos = newpos;
	return newpos;
}

static unsigned pid_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return (events & FS_POLL_READ) ? FS_POLL_READ : 0;
}

static int pid_release(file *fp)
{
	pid_file_data *data = fp->f_inode->i_private;
	proc_buf_free(data->buffer);
	free(data);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int pid_file_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	pid_file_data *data = node->i_private;
	proc_buf_t *pb = data->buffer;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_uid = data->uid;
	s->st_gid = data->gid;
	s->st_size = (loff_t)pb->len;
	s->st_blksize = PAGE_SIZE;
	s->st_nlink = 1;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static int pid_dir_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	pid_file_data *data = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_uid = data->uid;
	s->st_gid = data->gid;
	s->st_blksize = PAGE_SIZE;
	s->st_nlink = data->task_directory ?
			      2 + proc_thread_count(data->task_group) :
			      2;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static const file_operations pid_file_fops = {
	.getattr = pid_file_getattr,
	.read = pid_read,
	.llseek = pid_llseek,
	.poll = pid_poll,
	.release = pid_release,
};

static unsigned char pid_dirent_type(file *fp, const char *name)
{
	pid_file_data *data = fp->f_inode->i_private;
	if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "fd") ||
	    !strcmp(name, "task"))
		return S_IFDIR >> 12;
	if (data->task_directory && *name >= '0' && *name <= '9')
		return S_IFDIR >> 12;
	if (!strcmp(name, "cwd") || !strcmp(name, "exe"))
		return S_IFLNK >> 12;
	return 0;
}

static const file_operations pid_dir_fops = {
	.getattr = pid_dir_getattr,
	.read = pid_read,
	.dirent_type = pid_dirent_type,
	.llseek = pid_llseek,
	.poll = pid_poll,
	.release = pid_release,
};

/* ── Symlink ops for /proc/{pid}/fd/{N} ──────────────────────────────── */

static int pid_symlink_getattr(file *fp, struct stat *s)
{
	inode *node = fp->f_inode;
	const char *target = (const char *)node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = target ? (loff_t)strlen(target) : 0;
	s->st_nlink = 1;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static int pid_symlink_release(file *fp)
{
	free(fp->f_inode->i_private); /* strdup'd target */
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations pid_symlink_fops = {
	.getattr = pid_symlink_getattr,
	.release = pid_symlink_release,
};

typedef struct {
	char *name;
	file *target;
} pid_fd_link;

static int pid_fd_link_getattr(file *fp, struct stat *s)
{
	pid_fd_link *link = fp->f_inode->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = fp->f_inode->i_mode;
	s->st_size = strlen(link->name);
	s->st_nlink = 1;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static file *pid_fd_link_follow(file *fp, int flags)
{
	pid_fd_link *link = fp->f_inode->i_private;
	if (link->target->f_fop && link->target->f_fop->reopen)
		return link->target->f_fop->reopen(link->target, flags);
	if (link->target->f_name && link->target->f_name[0] == '/')
		return vfs_open(CURRENT_TASK()->root, link->target->f_name,
				flags);
	return NULL;
}

static int pid_fd_link_release(file *fp)
{
	pid_fd_link *link = fp->f_inode->i_private;
	fs_put_file(link->target);
	free(link->name);
	free(link);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations pid_fd_link_fops = {
	.getattr = pid_fd_link_getattr,
	.follow_link = pid_fd_link_follow,
	.release = pid_fd_link_release,
};

/* ── Public constructors ─────────────────────────────────────────────── */

file *make_pid_file(proc_buf_t *pb, task_struct *task)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	nd->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
	nd->i_private = pid_data_new(pb, task);

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_file_fops;
	return fp;
}

file *make_pid_dir(proc_buf_t *pb, task_struct *task)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	nd->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR | S_IXGRP |
		     S_IXOTH;
	nd->i_private = pid_data_new(pb, task);

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_dir_fops;
	return fp;
}

file *make_pid_task_dir(proc_buf_t *pb, task_struct *task)
{
	file *fp = make_pid_dir(pb, task);
	pid_file_data *data = fp->f_inode->i_private;
	data->task_directory = 1;
	data->task_group = task->tgid;
	return fp;
}

file *make_pid_symlink(const char *target)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	nd->i_mode = S_IFLNK | S_IRWXU | S_IRWXG | S_IRWXO;
	nd->i_private = strdup(target ? target : "(unknown)");

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_symlink_fops;
	return fp;
}

file *make_pid_fd_symlink(const char *name, file *target)
{
	file *fp = make_pid_symlink(name);
	pid_fd_link *link = zalloc(sizeof(*link));
	if (!link) {
		fs_put_file(fp);
		return NULL;
	}
	link->name = fp->f_inode->i_private;
	link->target = target;
	fs_get_file(target);
	fp->f_inode->i_private = link;
	fp->f_fop = &pid_fd_link_fops;
	return fp;
}

/* ── State helpers ───────────────────────────────────────────────────── */

char pid_state_char(ps_status st)
{
	switch (st) {
	case ps_running:
		return 'R';
	case ps_ready:
		return 'R';
	case ps_waiting:
		return 'S';
	case ps_dying:
		return 'Z';
	default:
		return '?';
	}
}

const char *pid_state_name(ps_status st)
{
	switch (st) {
	case ps_running:
		return "running";
	case ps_ready:
		return "runnable";
	case ps_waiting:
		return "sleeping";
	case ps_dying:
		return "zombie";
	default:
		return "unknown";
	}
}
