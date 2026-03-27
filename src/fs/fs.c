#include <fs/vfs.h>
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
#include <errno.h>

/*
 * fs_check_perm — check whether the current process may access a file.
 *
 * @s:    stat of the target file
 * @mask: requested access: R_OK (4), W_OK (2), X_OK (1), or 0 for existence
 *
 * Returns 0 if allowed, -EACCES if denied.
 * Root (euid == 0) bypasses DAC checks, except execute on a file requires
 * at least one execute bit to be set.
 */
int fs_check_perm(const struct stat *s, int mask)
{
	task_struct *cur = CURRENT_TASK();
	unsigned euid, egid, mode;
	int shift;

	if (!cur->user)
		return 0; /* kernel task — always allowed */

	euid = cur->user->euid;
	egid = cur->user->egid;
	mode = s->st_mode;

	if (euid == 0) {
		/* Root may read/write anything; execute only if some x bit set */
		if ((mask & X_OK) && !(mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
			return -EACCES;
		return 0;
	}

	if (euid == s->st_uid)
		shift = 6; /* use owner bits */
	else if (egid == s->st_gid)
		shift = 3; /* use group bits */
	else
		shift = 0; /* use other bits */

	if ((mask & R_OK) && !((mode >> shift) & S_IROTH))
		return -EACCES;
	if ((mask & W_OK) && !((mode >> shift) & S_IWOTH))
		return -EACCES;
	if ((mask & X_OK) && !((mode >> shift) & S_IXOTH))
		return -EACCES;

	return 0;
}

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
	return (int)n;
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

	if (!cur->root)
		return NULL;

	fp = vfs_open(cur->root, path, flag);

	/* Follow symlinks (e.g. /proc/{pid}/fd/{N}) unless O_NOFOLLOW. */
	if (fp && follow_link && !(flag & O_NOFOLLOW) && fp->f_inode &&
	    S_ISLNK(fp->f_inode->i_mode)) {
		const char *target = (const char *)fp->f_inode->i_private;
		if (target && target[0] == '/') {
			char *t = strdup(target);
			fs_put_file(fp);
			fp = vfs_open(cur->root, t, flag);
			free(t);
		} else {
			fs_put_file(fp);
			fp = NULL;
		}
	}

	if (fp && !fp->f_name)
		fp->f_name = strdup(path);

	return fp;
}

int fs_install_fd(file *fp, int flag)
{
	task_struct *cur = CURRENT_TASK();
	int fd;
	mutex_lock(&cur->fd_lock);
	fd = fs_find_empty_fd(cur->fds);
	if (fd >= 0) {
		cur->fds[fd].fp = fp;
		cur->fds[fd].flag = flag;
		cur->fds[fd].used = 1;
	}
	mutex_unlock(&cur->fd_lock);
	return fd;
}

int fs_open(const char *path, int flag, char *mode)
{
	file *fp = fs_open_file(path, flag, mode, 1);
	struct stat s;
	int acc, ret;

	if (!fp)
		return -ENOENT;

	/* Check DAC permissions based on requested access mode. */
	if (fp->f_inode && fp->f_inode->i_op && fp->f_inode->i_op->getattr &&
	    fp->f_inode->i_op->getattr(fp->f_inode, &s) == 0) {
		acc = 0;
		if ((flag & O_ACCMODE) != O_WRONLY)
			acc |= R_OK;
		if ((flag & O_ACCMODE) != O_RDONLY)
			acc |= W_OK;
		ret = fs_check_perm(&s, acc);
		if (ret) {
			fs_put_file(fp);
			return ret;
		}
	}

	fp->f_mode = (unsigned)(flag & O_ACCMODE);

	int fd = fs_install_fd(fp, flag);
	if (fd < 0)
		fs_put_file(fp);
	return fd < 0 ? -ENOENT : fd;
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

int fs_stat(const char *path, struct stat *s)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = vfs_open(cur->root, path, O_RDONLY);
	int ret;

	if (!fp)
		return -ENOENT;

	if (!fp->f_inode || !fp->f_inode->i_op || !fp->f_inode->i_op->getattr) {
		fs_put_file(fp);
		return -EACCES;
	}

	ret = fp->f_inode->i_op->getattr(fp->f_inode, s);
	fs_put_file(fp);
	return ret == EOK ? 0 : -1;
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
	file *fp[2] = { NULL, NULL };
	int reader, writer;

	if (pipe_open(fp) != EOK)
		return -1;

	reader = fs_install_fd(fp[0], O_RDONLY);
	if (reader < 0) {
		fs_put_file(fp[0]);
		fs_put_file(fp[1]);
		return -1;
	}

	writer = fs_install_fd(fp[1], O_WRONLY);
	if (writer < 0) {
		fs_close(reader);
		fs_put_file(fp[1]);
		return -1;
	}

	pipefd[0] = reader;
	pipefd[1] = writer;
	return 0;
}

int fs_dup(int fd)
{
	task_struct *cur = CURRENT_TASK();
	if (fd < 0 || fd >= MAX_FD || !cur->fds[fd].used)
		return -ENOENT;

	file *fp = cur->fds[fd].fp;
	int flag = cur->fds[fd].flag & ~O_CLOEXEC;
	fs_get_file(fp);

	int newfd = fs_install_fd(fp, flag);
	if (newfd < 0)
		fs_put_file(fp);
	return newfd < 0 ? -1 : newfd;
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
	task_struct *cur = CURRENT_TASK();
	file *fp = vfs_open(cur->root, pathname, O_RDONLY);
	struct stat s;
	int ret;

	if (!fp)
		return -ENOENT;

	if (!fp->f_inode || !fp->f_inode->i_op || !fp->f_inode->i_op->setattr) {
		fs_put_file(fp);
		return -EACCES;
	}

	/* Only file owner or root may chmod */
	if (fp->f_inode->i_op->getattr &&
	    fp->f_inode->i_op->getattr(fp->f_inode, &s) == 0) {
		if (cur->user && cur->user->euid != 0 &&
		    cur->user->euid != s.st_uid) {
			fs_put_file(fp);
			return -EPERM;
		}
	}

	ret = fp->f_inode->i_op->setattr(fp->f_inode, mode);
	fs_put_file(fp);
	return (0 - ret);
}

int fs_chown(const char *pathname, uint32_t uid, uint32_t gid)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = vfs_open(cur->root, pathname, O_RDONLY);
	struct stat s;
	int ret;

	if (!fp)
		return -ENOENT;

	if (!fp->f_inode || !fp->f_inode->i_op || !fp->f_inode->i_op->chown) {
		fs_put_file(fp);
		return -EACCES;
	}

	/* Only root may change owner; owner may change group to own group */
	if (cur->user) {
		if (cur->user->euid != 0) {
			/* non-root: may only change group, not owner */
			if (uid != (uint32_t)-1 && uid != s.st_uid) {
				fs_put_file(fp);
				return -EPERM;
			}
			if (fp->f_inode->i_op->getattr &&
			    fp->f_inode->i_op->getattr(fp->f_inode, &s) == 0) {
				if (cur->user->euid != s.st_uid) {
					fs_put_file(fp);
					return -EPERM;
				}
				if (gid != (uint32_t)-1 &&
				    gid != cur->user->egid &&
				    gid != cur->user->gid) {
					fs_put_file(fp);
					return -EPERM;
				}
			}
		}
	}

	ret = fp->f_inode->i_op->chown(fp->f_inode, uid, gid);
	fs_put_file(fp);
	return (0 - ret);
}

int fs_fchown(int fd, uint32_t uid, uint32_t gid)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	struct stat s;
	int ret = -EACCES;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -ENOENT;

	mutex_lock(&cur->fd_lock);
	fp = cur->fds[fd].fp;
	mutex_unlock(&cur->fd_lock);

	if (!fp || !fp->f_inode || !fp->f_inode->i_op ||
	    !fp->f_inode->i_op->chown)
		return -EACCES;

	/* Only root may change owner; owner may change group to own group */
	if (cur->user && cur->user->euid != 0) {
		if (fp->f_inode->i_op->getattr &&
		    fp->f_inode->i_op->getattr(fp->f_inode, &s) == 0) {
			if (uid != (uint32_t)-1 && uid != s.st_uid)
				return -EPERM;
			if (cur->user->euid != s.st_uid)
				return -EPERM;
			if (gid != (uint32_t)-1 && gid != cur->user->egid &&
			    gid != cur->user->gid)
				return -EPERM;
		}
	}

	ret = fp->f_inode->i_op->chown(fp->f_inode, uid, gid);
	return (0 - ret);
}

int fs_fchmod(int fd, uint32_t mode)
{
	task_struct *cur = CURRENT_TASK();
	file *fp = NULL;
	struct stat s;
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

	/* Only file owner or root may fchmod */
	if (cur->user && cur->user->euid != 0 && fp->f_inode->i_op->getattr &&
	    fp->f_inode->i_op->getattr(fp->f_inode, &s) == 0) {
		if (cur->user->euid != s.st_uid)
			return -EPERM;
	}

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
