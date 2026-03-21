/*
 * syscall_io.c — file descriptor I/O syscall handlers.
 *
 * Covers: read, write, open, close, ioctl, lseek, llseek,
 *         readv, writev, fsync, dup, dup2, pipe, fcntl,
 *         select, newselect, poll, readdir, getdents.
 */

#include <ps/ps.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <fs/select.h>
#include <fs/poll.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include "syscall_internal.h"

int sys_read(int fd, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();

	if (fd < 0 || fd >= MAX_FD)
		return -1;
	if (cur->fds[fd].used == 0)
		return -1;
	if (S_ISDIR(cur->fds[fd].fp->f_inode->i_mode))
		return -1;

	if (TestControl.verbos)
		klog("read(%d, %x, %d)\n", fd, buf, len);

	return fs_read(fd, -1, buf, len);
}

int sys_write(int fd, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();

	if (TestControl.verbos)
		klog("write(%d, %x, %d)\n", fd, buf, len);

	if (fd < 0 || fd >= MAX_FD)
		return -1;
	if (cur->fds[fd].used == 0)
		return -1;
	if (S_ISDIR(cur->fds[fd].fp->f_inode->i_mode))
		return -1;

	return fs_write(fd, -1, buf, len);
}

int sys_ioctl(int fd, int request, char *buf)
{
	int ret = fs_ioctl(fd, request, buf);

	if (TestControl.verbos)
		klog("ioctl(%d, %x, ...) = %d\n", fd, request, ret);

	return ret;
}

int sys_open(const char *_name, int flags, char *mode)
{
	char *name = name_get();
	int fd;

	resolve_path(_name, name);
	fd = fs_open(name, flags, mode);

	if (TestControl.verbos)
		klog("open(%s, %x, %x) = %d\n", name, flags, mode, fd);

	name_put(name);
	return fd;
}

int sys_close(unsigned fd)
{
	if (TestControl.verbos)
		klog("close(%d)\n", fd);

	return fs_close(fd);
}

int sys_lseek(int fd, int offset, int whence)
{
	int ret = fs_seek(fd, offset, whence);

	if (TestControl.verbos)
		klog("lseek(%d, %d, %d) = %d\n", fd, offset, whence, ret);

	return ret;
}

int sys_llseek(int fd, unsigned offset_high, unsigned offset_low,
	       uint64_t *result, unsigned whence)
{
	int ret = fs_llseek(fd, offset_high, offset_low, result, whence);

	if (TestControl.verbos)
		klog("llseek(%d, %x, %x, %x, %d) = %d, current %d\n", fd,
		     offset_high, offset_low, result, whence, ret,
		     (int)CURRENT_TASK()->fds[fd].fp->f_pos);

	return ret;
}

int sys_readv(int fildes, const struct iovec *iov, int iovcnt)
{
	int i;
	unsigned total = 0;

	if (TestControl.verbos)
		klog("readv(%d, %x, %d)\n", fildes, iov, iovcnt);

	for (i = 0; i < iovcnt; i++) {
		total += iov[i].iov_len;
		memset(iov[i].iov_base, 0, iov[i].iov_len);
	}
	return total;
}

int sys_writev(int fildes, const struct iovec *iov, int iovcnt)
{
	int i;
	unsigned total = 0;

	if (TestControl.verbos)
		klog("writev: fd %d\n", fildes);

	for (i = 0; i < iovcnt; i++)
		total += sys_write(fildes, iov[i].iov_base, iov[i].iov_len);

	return total;
}

int sys_fsync(int fd)
{
	if (TestControl.verbos)
		klog("sys_fsync(%d)\n", fd);

	return fs_sync(fd);
}

int sys_dup(int oldfd)
{
	int ret;

	if (oldfd == -1 || oldfd >= MAX_FD)
		return -1;

	ret = fs_dup(oldfd);

	if (TestControl.verbos)
		klog("dup(%d) = %d\n", oldfd, ret);

	return ret;
}

int sys_dup2(int oldfd, int newfd)
{
	if (TestControl.verbos)
		klog("dup2(%d, %d)\n", oldfd, newfd);

	if (oldfd == -1 || newfd == -1)
		return -1;
	if (oldfd >= MAX_FD || newfd >= MAX_FD)
		return -1;
	if (oldfd == newfd)
		return -1;

	return fs_dup2(oldfd, newfd);
}

int sys_pipe(int pipefd[2])
{
	int ret = fs_pipe(pipefd);

	if (TestControl.verbos)
		klog("pipe(pipefd[%d,%d]) = %d\n", pipefd[0], pipefd[1], ret);

	return ret;
}

int sys_fcntl(int fd, int cmd, int arg)
{
	task_struct *cur = CURRENT_TASK();
	int ret = 0;

	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;
	if (cur->fds[fd].used == 0)
		return -ENOENT;

	switch (cmd) {
	case F_DUPFD:
		ret = fs_dup(fd);
		break;
	case F_GETFD:
		ret = (cur->fds[fd].flag & O_CLOEXEC) ? FD_CLOEXEC : 0;
		break;
	case F_SETFD:
		if (arg & FD_CLOEXEC)
			cur->fds[fd].flag |= O_CLOEXEC;
		else
			cur->fds[fd].flag &= ~O_CLOEXEC;
		ret = 0;
		break;
	case F_GETFL:
		ret = cur->fds[fd].flag;
		break;
	case F_SETFL:
		cur->fds[fd].flag = arg;
		ret = 0;
		break;
	default:
		ret = 0;
		break;
	}

	if (TestControl.verbos)
		klog("fcntl(%d, %d, %d) = %d\n", fd, cmd, arg, ret);

	return ret;
}

int sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count)
{
	struct stat s;
	file *fp;
	ssize_t n;

	if (TestControl.verbos)
		klog("getdents(%d, %x, %d)\n", fd, dirp, count);

	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;
	if (fs_fstat(fd, &s) != EOK)
		return -ENOENT;
	if (!S_ISDIR(s.st_mode))
		return -EISDIR;

	fp = CURRENT_TASK()->fds[fd].fp;

	if (count < sizeof(struct linux_dirent))
		return -22;
	if (!fp->f_fop || !fp->f_fop->read)
		return -1;

	n = fp->f_fop->read(fp, dirp, count, &fp->f_pos);
	if (n < 0)
		return -1;
	return (size_t)n;
}

int sys_readdir(unsigned fd, struct linux_dirent *dirp, unsigned count)
{
	if (TestControl.verbos)
		klog("readdir(%d, %x, %d)\n", fd, dirp, count);

	return sys_getdents(fd, dirp, count);
}

int sys_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	       const struct timespec *timeout)
{
	if (TestControl.verbos)
		klog("select(%d, %x, %x, %x, %x)\n", nfds, readfds, writefds,
		     exceptfds, timeout);

	return do_select(nfds, readfds, writefds, exceptfds, timeout, NULL);
}

int sys_newselect(int nfds, fd_set *readfds, fd_set *writefds,
		  fd_set *exceptfds, const struct timespec *timeout,
		  void *sigmask)
{
	if (TestControl.verbos)
		klog("pselect(%d, %x, %x, %x, %x, %x)\n", nfds, readfds,
		     writefds, exceptfds, timeout, sigmask);

	return do_select(nfds, readfds, writefds, exceptfds, timeout, sigmask);
}

int sys_poll(struct pollfd *fds, unsigned nfds, int timeout)
{
	if (TestControl.verbos)
		klog("poll(%x, %d, %d)\n", fds, nfds, timeout);

	return do_poll(fds, nfds, timeout);
}
