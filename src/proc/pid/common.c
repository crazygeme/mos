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

static ssize_t pid_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	proc_buf_t *pb = fp->f_inode->i_private;
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
	proc_buf_t *pb = fp->f_inode->i_private;
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

static int pid_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT || type == FS_POLL_WRITE)
		return -1;
	return 0;
}

static int pid_release(file *fp)
{
	proc_buf_free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int pid_file_getattr(inode *node, struct stat *s)
{
	proc_buf_t *pb = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = (loff_t)pb->len;
	s->st_blksize = PAGE_SIZE;
	s->st_nlink = 1;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static int pid_dir_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_nlink = 2;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static const inode_operations pid_file_iops = { .getattr = pid_file_getattr };
static const inode_operations pid_dir_iops = { .getattr = pid_dir_getattr };

static const file_operations pid_file_fops = {
	.read = pid_read,
	.llseek = pid_llseek,
	.poll = pid_poll,
	.release = pid_release,
};

static const file_operations pid_dir_fops = {
	.read = pid_read,
	.llseek = pid_llseek,
	.poll = pid_poll,
	.release = pid_release,
};

/* ── Symlink ops for /proc/{pid}/fd/{N} ──────────────────────────────── */

static int pid_symlink_getattr(inode *node, struct stat *s)
{
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

static const inode_operations pid_symlink_iops = {
	.getattr = pid_symlink_getattr,
};
static const file_operations pid_symlink_fops = {
	.release = pid_symlink_release,
};

/* ── Public constructors ─────────────────────────────────────────────── */

file *make_pid_file(proc_buf_t *pb)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	nd->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
	nd->i_op = &pid_file_iops;
	nd->i_private = pb;

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_file_fops;
	return fp;
}

file *make_pid_dir(proc_buf_t *pb)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	nd->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR | S_IXGRP |
		     S_IXOTH;
	nd->i_op = &pid_dir_iops;
	nd->i_private = pb;

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_dir_fops;
	return fp;
}

file *make_pid_symlink(const char *target)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	nd->i_mode = S_IFLNK | S_IRWXU | S_IRWXG | S_IRWXO;
	nd->i_op = &pid_symlink_iops;
	nd->i_private = strdup(target ? target : "(unknown)");

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_symlink_fops;
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
