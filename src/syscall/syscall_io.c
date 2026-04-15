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

#define SHOW_CHARS 128 /* how many chars to show in strace logs */
static char *format_buffer(const char *buf, unsigned len)
{
	/* Format like strace: at most SHOW_CHARS chars, escape non-printables */
	static const char hex[] = "0123456789abcdef";
	char *tmp = malloc(4 * SHOW_CHARS +
			   6); /* '"' + SHOW_CHARS*4 + '"' + "..." + NUL */
	unsigned i, n = len < SHOW_CHARS ? len : SHOW_CHARS;
	char *p = tmp;
	*p++ = '"';
	for (i = 0; buf && i < n; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (c == '\n') {
			*p++ = '\\';
			*p++ = 'n';
		} else if (c == '\t') {
			*p++ = '\\';
			*p++ = 't';
		} else if (c == '\r') {
			*p++ = '\\';
			*p++ = 'r';
		} else if (c == '\\') {
			*p++ = '\\';
			*p++ = '\\';
		} else if (c == '"') {
			*p++ = '\\';
			*p++ = '"';
		} else if (c == '\0') {
			*p++ = '\\';
			*p++ = '0';
		} else if (c >= 32 && c < 127) {
			*p++ = (char)c;
		} else {
			*p++ = '\\';
			*p++ = 'x';
			*p++ = hex[c >> 4];
			*p++ = hex[c & 0xf];
		}
	}
	*p++ = '"';
	if (len > SHOW_CHARS) {
		*p++ = '.';
		*p++ = '.';
		*p++ = '.';
	}
	*p = '\0';
	return tmp;
}

int sys_read(int fd, char *buf, unsigned len)
{
	int ret = -EBADF;
	task_struct *cur = CURRENT_TASK();

	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;
	if (cur->fds[fd] == NULL)
		return -EBADF;
	if (S_ISDIR(cur->fds[fd]->f_inode->i_mode))
		return -EISDIR;

	ret = fs_read(fd, -1, buf, len);

	if (TEST_LOG(TEST_LOG_INFO)) {
		char *tmp = format_buffer(buf, ret > 0 ? ret : 0);
		klog("read(%d, %s, %d) = %d\n", fd, tmp, len, ret);
		free(tmp);
	}
	return ret;
}

int sys_write(int fd, const char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_INFO)) {
		char *tmp = format_buffer(buf, len);
		klog("write(%d, %s, %d)\n", fd, tmp, len);
		free(tmp);
	}

	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;
	if (cur->fds[fd] == NULL)
		return -EBADF;
	if (S_ISDIR(cur->fds[fd]->f_inode->i_mode))
		return -EISDIR;

	return fs_write(fd, -1, buf, len);
}

int sys_pread64(int fd, void *buf, unsigned count, int offset)
{
	task_struct *cur = CURRENT_TASK();
	int ret = -EBADF;

	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;
	if (cur->fds[fd] == NULL)
		return -EBADF;
	if (S_ISDIR(cur->fds[fd]->f_inode->i_mode))
		return -EISDIR;

	ret = fs_pread(fd, offset, buf, count);

	if (TEST_LOG(TEST_LOG_TRACE)) {
		char *tmp = format_buffer(buf, count);
		klog("pread(%d, %s, %d, %d) = %d\n", fd, tmp, count, offset,
		     ret > 0 ? ret : 0);
		free(tmp);
	}

	return ret;
}

int sys_pwrite64(int fd, const void *buf, unsigned count, int offset)
{
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE)) {
		char *tmp = format_buffer(buf, count);
		klog("write(%d, %s, %d, %d)\n", fd, tmp, count, offset);
		free(tmp);
	}

	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;
	if (cur->fds[fd] == NULL)
		return -EBADF;
	if (S_ISDIR(cur->fds[fd]->f_inode->i_mode))
		return -EISDIR;

	return fs_pwrite(fd, offset, buf, count);
}

int sys_ioctl(int fd, int request, char *buf)
{
	int ret = fs_ioctl(fd, request, buf);

	if (TEST_LOG(TEST_LOG_TRACE) && request != 0x4b46 &&
	    request != 0x4b47 && request != 0x4b48 && request != 0x4b49)
		klog("ioctl(%d, %x, ...) = %d\n", fd, request, ret);

	return ret;
}

int sys_open(const char *_name, int flags, umode_t mode)
{
	char *name = name_get();
	int fd;

	resolve_path(_name, name);

	fd = fs_open(name, flags, mode);

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("open(%s, %x, %x) = %d\n", name, flags, mode, fd);

	name_put(name);
	return fd;
}

int sys_close(unsigned fd)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("close(%d)\n", fd);

	return fs_close(fd);
}

int sys_lseek(int fd, int offset, int whence)
{
	int ret = fs_seek(fd, offset, whence);

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("lseek(%d, %d, %d) = %d\n", fd, offset, whence, ret);

	return ret;
}

int sys_llseek(int fd, unsigned offset_high, unsigned offset_low,
	       uint64_t *result, unsigned whence)
{
	int ret = fs_llseek(fd, offset_high, offset_low, result, whence);

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("llseek(%d, %x, %x, %x, %d) = %d, current %d\n", fd,
		     offset_high, offset_low, result, whence, ret,
		     (int)CURRENT_TASK()->fds[fd]->f_pos);

	return ret;
}

int sys_readv(int fildes, const struct iovec *iov, int iovcnt)
{
	int i;
	int ret;
	size_t total_len = 0;
	size_t copied = 0;
	task_struct *cur = CURRENT_TASK();
	file *fp;
	char *buf;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("readv(%d, %x, %d)\n", fildes, iov, iovcnt);

	if (fildes < 0 || fildes >= MAX_FD)
		return -EBADF;
	if (iovcnt < 0)
		return -EINVAL;
	if (cur->fds[fildes] == NULL)
		return -EBADF;

	fp = cur->fds[fildes];
	if (!fp || !fp->f_fop || !fp->f_fop->read)
		return -EBADF;
	if (S_ISDIR(fp->f_inode->i_mode))
		return -EISDIR;

	for (i = 0; i < iovcnt; i++)
		total_len += iov[i].iov_len;

	if (total_len == 0)
		return 0;

	buf = malloc(total_len);
	if (!buf)
		return -ENOMEM;

	ret = (int)fp->f_fop->read(fp, buf, total_len, &fp->f_pos);
	if (ret <= 0) {
		free(buf);
		return ret;
	}

	for (i = 0; i < iovcnt && copied < (size_t)ret; i++) {
		size_t n = iov[i].iov_len;
		if (n > (size_t)ret - copied)
			n = (size_t)ret - copied;
		if (n > 0)
			memcpy(iov[i].iov_base, buf + copied, n);
		copied += n;
	}

	free(buf);
	return ret;
}

int sys_writev(int fildes, const struct iovec *iov, int iovcnt)
{
	int i;
	int ret;
	size_t total_len = 0;
	size_t copied = 0;
	task_struct *cur = CURRENT_TASK();
	file *fp;
	char *buf;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("writev: fd %d\n", fildes);

	if (fildes < 0 || fildes >= MAX_FD)
		return -EBADF;
	if (iovcnt < 0)
		return -EINVAL;
	if (cur->fds[fildes] == NULL)
		return -EBADF;

	fp = cur->fds[fildes];
	if (!fp || !fp->f_fop || !fp->f_fop->write)
		return -EBADF;
	if (S_ISDIR(fp->f_inode->i_mode))
		return -EISDIR;

	for (i = 0; i < iovcnt; i++)
		total_len += iov[i].iov_len;

	if (total_len == 0)
		return 0;

	buf = malloc(total_len);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0)
			continue;
		memcpy(buf + copied, iov[i].iov_base, iov[i].iov_len);
		copied += iov[i].iov_len;
	}

	ret = (int)fp->f_fop->write(fp, buf, total_len, &fp->f_pos);
	free(buf);
	return ret;
}

int sys_fsync(int fd)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("sys_fsync(%d)\n", fd);

	return fs_sync(fd);
}

int sys_dup(int oldfd)
{
	int ret;

	if (oldfd == -1 || oldfd >= MAX_FD)
		return -1;

	ret = fs_dup(oldfd);

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("dup(%d) = %d\n", oldfd, ret);

	return ret;
}

int sys_dup2(int oldfd, int newfd)
{
	if (TEST_LOG(TEST_LOG_TRACE))
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

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("pipe(pipefd[%d,%d]) = %d\n", pipefd[0], pipefd[1], ret);

	return ret;
}

int sys_fcntl(int fd, int cmd, int arg)
{
	task_struct *cur = CURRENT_TASK();
	int ret = 0;

	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;
	if (cur->fds[fd] == NULL)
		return -ENOENT;

	switch (cmd) {
	case F_DUPFD:
		ret = fs_dup(fd);
		break;
	case F_GETFD:
		ret = fd_bitmap_test(cur->fd_cloexec, fd) ? FD_CLOEXEC : 0;
		break;
	case F_SETFD:
		if (arg & FD_CLOEXEC)
			fd_bitmap_set(cur->fd_cloexec, fd);
		else
			fd_bitmap_clear(cur->fd_cloexec, fd);
		ret = 0;
		break;
	case F_GETFL:
		ret = cur->fds[fd]->f_flag;
		break;
	case F_SETFL:
		cur->fds[fd]->f_flag = (cur->fds[fd]->f_flag & O_ACCMODE) |
				       (arg & ~(O_ACCMODE | O_CLOEXEC));
		ret = 0;
		break;
	case F_SETOWN:
		cur->fds[fd]->f_owner = arg;
		ret = 0;
		break;
	case F_GETOWN:
		ret = cur->fds[fd]->f_owner;
		break;
	case F_SETSIG:
		cur->fds[fd]->f_sigio = arg;
		ret = 0;
		break;
	case F_GETSIG:
		ret = cur->fds[fd]->f_sigio;
		break;
	default:
		ret = 0;
		break;
	}

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("fcntl(%d, %d, %d) = %d\n", fd, cmd, arg, ret);

	return ret;
}

int sys_fcntl64(int fd, int cmd, int arg)
{
	switch (cmd) {
	case F_GETLK64:
	case F_SETLK64:
	case F_SETLKW64:
		return 0;
	default:
		return sys_fcntl(fd, cmd, arg);
	}
}

int sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count)
{
	struct stat s;
	file *fp;
	ssize_t n;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getdents(%d, %x, %d)\n", fd, dirp, count);

	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;
	if (fs_fstat(fd, &s) != EOK)
		return -ENOENT;
	if (!S_ISDIR(s.st_mode))
		return -EISDIR;

	fp = CURRENT_TASK()->fds[fd];

	if (count < sizeof(struct linux_dirent))
		return -22;
	if (!fp->f_fop || !fp->f_fop->read)
		return -1;

	n = fp->f_fop->read(fp, dirp, count, &fp->f_pos);
	if (n < 0)
		return -1;
	return (size_t)n;
}

int sys_getdents64(unsigned int fd, struct linux_dirent64 *dirp,
		   unsigned int count)
{
	struct stat s;
	file *fp;
	char *tmp;
	ssize_t n;
	char *src, *dst;
	int out;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("getdents64(%d, %x, %d)\n", fd, dirp, count);

	if (fd < 0 || fd >= MAX_FD)
		return -ENOENT;
	if (fs_fstat(fd, &s) != EOK)
		return -ENOENT;
	if (!S_ISDIR(s.st_mode))
		return -EISDIR;

	fp = CURRENT_TASK()->fds[fd];

	if (count < sizeof(struct linux_dirent64))
		return -22;
	if (!fp->f_fop || !fp->f_fop->read)
		return -1;

	tmp = malloc(count);
	if (!tmp)
		return -ENOMEM;

	n = fp->f_fop->read(fp, tmp, count, &fp->f_pos);
	if (n < 0) {
		free(tmp);
		return -1;
	}

	src = tmp;
	dst = (char *)dirp;
	out = 0;
	while (src < tmp + n) {
		struct linux_dirent *d = (struct linux_dirent *)src;
		struct linux_dirent64 *d64 = (struct linux_dirent64 *)dst;
		unsigned namelen = strlen(d->d_name);
		unsigned reclen64 = ROUND_UP(NAME64_OFFSET() + namelen + 1);

		if (out + (int)reclen64 > (int)count)
			break;

		d64->d_ino = d->d_ino;
		d64->d_off = d->d_off;
		d64->d_reclen = reclen64;
		d64->d_type = 0; /* DT_UNKNOWN */
		memcpy(d64->d_name, d->d_name, namelen + 1);

		src += d->d_reclen;
		dst += reclen64;
		out += reclen64;
	}

	free(tmp);
	return out;
}

int sys_readdir(unsigned fd, struct linux_dirent *dirp, unsigned count)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("readdir(%d, %x, %d)\n", fd, dirp, count);

	return sys_getdents(fd, dirp, count);
}

int sys_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	       const struct timeval *timeout)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("select(%d, %x, %x, %x, %x)\n", nfds, readfds, writefds,
		     exceptfds, timeout);

	return do_select(nfds, readfds, writefds, exceptfds, timeout, NULL);
}

int sys_newselect(int nfds, fd_set *readfds, fd_set *writefds,
		  fd_set *exceptfds, const struct timeval *timeout,
		  void *sigmask)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("_newselect(%d, %x, %x, %x, %x)\n", nfds, readfds,
		     writefds, exceptfds, timeout);

	/*
	 * Linux i386 __NR__newselect (142) is plain select(2), not pselect(2).
	 * The kernel entry gets only the five select arguments; any extra
	 * register value here is not a user-provided sigmask and must be ignored.
	 */
	(void)sigmask;
	return do_select(nfds, readfds, writefds, exceptfds, timeout, NULL);
}

int sys_poll(struct pollfd *fds, unsigned nfds, int timeout)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("poll(%x, %d, %d)\n", fds, nfds, timeout);

	return do_poll(fds, nfds, timeout);
}
