
#include "ext4_errno.h"
#include <fs/mount.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/pipe.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <ps/ps.h>
#include <hw/hdd.h>
#include <ext4.h>
#include <unistd.h>
#include <macro.h>

static int fs_find_empty_fd(file_descriptor *fds)
{
	int i;
	for (i = 0; i < MAX_FD; i++) {
		if (fds[i].used == 0)
			return i;
	}
	return -1;
}

int fs_read(int fd, unsigned offset, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	ssize_t n;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_fop || !fp->f_fop->read)
		return -1;

	if (offset != (unsigned)-1)
		fp->f_pos = offset;

	n = fp->f_fop->read(fp, buf, len, &fp->f_pos);
	return n < 0 ? -1 : (int)n;
}

int fs_write(int fd, unsigned offset, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	ssize_t n;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_fop || !fp->f_fop->write)
		return -1;

	if (offset != (unsigned)-1)
		fp->f_pos = offset;

	n = fp->f_fop->write(fp, buf, len, &fp->f_pos);
	return n < 0 ? -1 : (int)n;
}

file *fs_open_file(const char *path, int flag, char *mode, int follow_link)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;

	if (cur->root)
		fp = vfs_open(cur->root, path, flag);

	if (fp)
		fp->f_name = strdup(path);

	return fp;
}

int fs_open(const char *path, int flag, char *mode)
{
	task_struct *cur = CURRENT_TASK();
	int fd = -1;
	file *fp = NULL;
	int ret = -ENOENT;
	mutex_lock(&cur->fd_lock);

	fd = fs_find_empty_fd(cur->fds);
	if (fd < 0)
		goto done;

	fp = fs_open_file(path, flag, mode, 1);
	if (!fp)
		goto fail;

	fp->f_mode = (unsigned)(flag & O_ACCMODE);
	cur->fds[fd].flag = flag;
	cur->fds[fd].fp = fp;
	cur->fds[fd].used = 1;

	goto done;
fail:
	fd = ret;
done:
	mutex_unlock(&cur->fd_lock);
	return fd;
}

int fs_close(int fd)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	memset(&cur->fds[fd], 0, sizeof(file_descriptor));
	mutex_unlock(&cur->fd_lock);

	if (fp == NULL)
		return -1;

	return fs_put_file(fp);
}

int fs_delete(const char *path)
{
	/* FIXME: before virtual device (/dev etc) only ext4 support */
	int ret = ext4_fremove(path);
	if (ret != EOK)
		return -1;
	return ret;
}

int fs_stat(const char *path, struct stat *s)
{
	/* FIXME: before virtual device (/dev etc) only ext4 support */
	ext4_file f;
	ext4_dir dir;
	int isdir = 0;
	int ret = -1;

	if (path[strlen(path) - 1] == '/') {
		ret = ext4_dir_open(&dir, path);
		isdir = 1;
	} else {
		ret = ext4_fopen(&f, path, "r");
	}

	if (ret != EOK)
		return (0 - ret);

	if (!isdir) {
		ret = ext4_fstat(&f, s);
		ext4_fclose(&f);
	} else {
		ret = ext4_fstat(&dir.f, s);
		ext4_dir_close(&dir);
	}

	if (ret != EOK)
		return -1;

	return ret;
}

int fs_fstat(int fd, struct stat *s)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	mutex_unlock(&cur->fd_lock);

	if (!fp || !fp->f_inode || !fp->f_inode->i_op ||
	    !fp->f_inode->i_op->getattr)
		return -1;

	ret = fp->f_inode->i_op->getattr(fp->f_inode, s);
	return ret == EOK ? 0 : -1;
}

int fs_pipe(int *pipefd)
{
	int ret = 0;
	task_struct *cur = CURRENT_TASK();
	int reader, writer;
	file *fp[2] = { 0 };
	mutex_lock(&cur->fd_lock);
	reader = fs_find_empty_fd(cur->fds);
	if (reader < 0 || reader >= MAX_FD) {
		ret = -1;
		goto done;
	}
	/* reserve slot for writer search */
	cur->fds[reader].used = 1;

	writer = fs_find_empty_fd(cur->fds);
	if (writer < 0 || writer >= MAX_FD) {
		cur->fds[reader].used = 0;
		ret = -1;
		goto done;
	}
	cur->fds[writer].used = 1;
	ret = pipe_open(fp);
	if (ret != EOK) {
		cur->fds[reader].used = 0;
		cur->fds[writer].used = 0;
		ret = -1;
		goto done;
	}

	cur->fds[reader].fp = fp[0];
	cur->fds[writer].fp = fp[1];
	cur->fds[reader].flag = O_RDONLY;
	cur->fds[writer].flag = O_WRONLY;
	pipefd[0] = reader;
	pipefd[1] = writer;
	ret = 0;
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_dup(int fd)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int newfd;
	int ret;
	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;
	if (cur->fds[fd].used == 0)
		return -ENOENT;
	mutex_lock(&cur->fd_lock);
	newfd = fs_find_empty_fd(cur->fds);
	if (newfd < 0 || newfd >= MAX_FD) {
		ret = -1;
		goto done;
	}
	cur->fds[newfd] = cur->fds[fd];
	fp = cur->fds[fd].fp;
	fs_get_file(fp);
	cur->fds[newfd].flag &= ~O_CLOEXEC;
	ret = newfd;
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_dup2(int fd, int newfd)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret;
	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;

	if (newfd < 0 || newfd >= MAX_FD)
		return -EACCES;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	if (cur->fds[newfd].used)
		fs_put_file(cur->fds[newfd].fp);
	fp = cur->fds[fd].fp;
	fs_get_file(fp);
	cur->fds[newfd] = cur->fds[fd];
	cur->fds[newfd].flag &= ~O_CLOEXEC;
	ret = newfd;
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_put_file(file *f)
{
	if (__sync_add_and_fetch(&f->f_count, -1) == 0) {
		if (f->f_name)
			free(f->f_name);
		if (f->f_fop && f->f_fop->release)
			f->f_fop->release(f);
		else {
			if (f->f_inode)
				free(f->f_inode);
			free(f);
		}
	}
	return 0;
}

int fs_llseek(int fd, unsigned offset_high, unsigned offset_low,
	      uint64_t *result, unsigned whence)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	loff_t offset = (loff_t)offset_high << 32 | (loff_t)offset_low;
	loff_t pos = -1;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_fop || !fp->f_fop->llseek)
		goto done;
	pos = fp->f_fop->llseek(fp, offset, whence);
	if (pos >= 0) {
		fp->f_pos = pos;
		if (result)
			*result = (uint64_t)pos;
	}
done:
	mutex_unlock(&cur->fd_lock);
	return (int)pos;
}

int fs_seek(int fd, int offset, unsigned whence)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	loff_t pos = -EACCES;
	int ret = 0;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_fop || !fp->f_fop->llseek)
		goto done;

	pos = fp->f_fop->llseek(fp, offset, whence);
	if (pos >= 0)
		fp->f_pos = pos;
done:
	mutex_unlock(&cur->fd_lock);
	return (int)pos;
}

int fs_sync(int fd)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret = -ENOENT;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;

	if (!fp || !fp->f_fop)
		goto done;

	if (fp->f_fop->flush)
		ret = fp->f_fop->flush(fp);
	else
		ret = 0;
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_select(int fd, unsigned type)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret = -EACCES;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_fop || !fp->f_fop->poll)
		goto done;

	ret = fp->f_fop->poll(fp, type);
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_ioctl(int fd, unsigned cmd, void *buf)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret = -EACCES;
	if (fd < 0 || fd >= MAX_FD)
		return -EINVAL;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	if (!fp || !fp->f_fop || !fp->f_fop->ioctl)
		goto done;

	ret = fp->f_fop->ioctl(fp, cmd, buf);
done:
	mutex_unlock(&cur->fd_lock);
	return ret;
}

int fs_chmod(const char *pathname, uint32_t mode)
{
	int ret;
	ret = ext4_chmod(pathname, mode);
	return (0 - ret);
}

int fs_fchmod(int fd, uint32_t mode)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	int ret = -EACCES;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	mutex_unlock(&cur->fd_lock);

	if (!fp || !fp->f_inode || !fp->f_inode->i_op ||
	    !fp->f_inode->i_op->setattr)
		return -EACCES;

	ret = fp->f_inode->i_op->setattr(fp->f_inode, mode);
	return (0 - ret);
}

/*
 * resolve_path - turn a user-supplied path into an absolute path.
 *
 * Handles:
 *   - NULL / empty input                     → error
 *   - Exact "."  / ".."                      → cwd / parent of cwd
 *   - Absolute paths (start with '/')        → copy, then strip trailing
 *                                              "/.." or "/." components
 *   - Relative paths (including "./" prefix) → prepend cwd
 *
 * The result is written into `new` (caller must supply at least MAX_PATH
 * bytes).  Returns 0 on success, -1 on error.
 */
int resolve_path(const char *old, char *new)
{
	char *r;
	int len;

	/* Null check must precede any use of old */
	if (!old || !*old)
		return -1;

	len = strlen(old);

	/* "." — current directory */
	if (!strcmp(old, ".")) {
		sys_getcwd(new, MAX_PATH);
		return 0;
	}

	/* ".." — parent of current directory */
	if (!strcmp(old, "..")) {
		sys_getcwd(new, MAX_PATH);
		r = strrchr(new, '/');
		if (r == new) {
			/* Already at root: stay at "/" */
			new[0] = '/';
			new[1] = '\0';
		} else {
			*r = '\0';
		}
		return 0;
	}

	if (old[0] == '/') {
		/* Absolute path: copy verbatim, then normalise trailing component */
		strcpy(new, old);

		if (len >= 3 && new[len - 1] == '.' && new[len - 2] == '.' &&
		    new[len - 3] == '/') {
			/* Trailing "/.." — strip it, then strip the last dir component.
			 * Two-step: first remove the "/.." suffix to get the parent
			 * dir string, then find the slash before that dir name. */
			new[len - 3] = '\0'; /* e.g. "/foo/bar"  */
			r = strrchr(new, '/');
			if (!r) {
				/* Path was something like "/bar/.." — result is root */
				new[0] = '/';
				new[1] = '\0';
			} else if (r == new) {
				/* Parent is root: "/foo/.." → "/" */
				new[1] = '\0';
			} else {
				/* General case: "/foo/bar/.." → "/foo/" */
				*(r + 1) = '\0';
			}
		} else if (len >= 2 && new[len - 1] == '.' &&
			   new[len - 2] == '/') {
			/* Trailing "/." — strip the dot, keep the slash */
			new[len - 1] = '\0';
		}
		return 0;
	}

	/* Relative path: strip leading "./" if present */
	if (len > 1 && old[0] == '.' && old[1] == '/')
		old += 2;

	/* Prepend cwd, ensuring a '/' separator between cwd and old */
	sys_getcwd(new, MAX_PATH);
	len = strlen(new);
	if (len > 0 && new[len - 1] != '/')
		new[len++] = '/';
	strcat(new + len, old);
	return 0;
}
