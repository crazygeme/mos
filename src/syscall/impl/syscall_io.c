/*
 * syscall_io.c — file descriptor I/O syscall handlers.
 *
 * Covers: read, write, open, close, ioctl, lseek, llseek,
 *         readv, writev, fsync, dup, dup2, pipe, fcntl,
 *         select, newselect, poll, readdir, getdents.
 */

#include <ps/ps.h>
#include <fs/fs.h>
#include <fs/inotify.h>
#include <fs/vfs.h>
#include <fs/select.h>
#include <fs/poll.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <net/sock.h>
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
	if (cur->files->fds[fd] == NULL)
		return -EBADF;
	if (S_ISDIR(cur->files->fds[fd]->f_inode->i_mode))
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
	if (cur->files->fds[fd] == NULL)
		return -EBADF;
	if (S_ISDIR(cur->files->fds[fd]->f_inode->i_mode))
		return -EISDIR;

	return fs_write(fd, -1, buf, len);
}

int sys_pread64(int fd, void *buf, unsigned count, unsigned offset_low,
		unsigned offset_high)
{
	loff_t offset = (loff_t)(((uint64_t)offset_high << 32) | offset_low);
	task_struct *cur = CURRENT_TASK();
	int ret = -EBADF;

	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;
	if (cur->files->fds[fd] == NULL)
		return -EBADF;
	if (S_ISDIR(cur->files->fds[fd]->f_inode->i_mode))
		return -EISDIR;

	ret = fs_pread(fd, offset, buf, count);

	if (TEST_LOG(TEST_LOG_TRACE)) {
		char *tmp = format_buffer(buf, ret > 0 ? (unsigned)ret : 0);
		klog("pread(%d, %s, %u, 0x%x%08x) = %d\n", fd, tmp, count,
		     offset_high, offset_low, ret);
		free(tmp);
	}

	return ret;
}

int sys_pwrite64(int fd, const void *buf, unsigned count, unsigned offset_low,
		 unsigned offset_high)
{
	loff_t offset = (loff_t)(((uint64_t)offset_high << 32) | offset_low);
	task_struct *cur = CURRENT_TASK();

	if (TEST_LOG(TEST_LOG_TRACE)) {
		char *tmp = format_buffer(buf, count);
		klog("pwrite(%d, %s, %u, 0x%x%08x)\n", fd, tmp, count,
		     offset_high, offset_low);
		free(tmp);
	}

	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;
	if (cur->files->fds[fd] == NULL)
		return -EBADF;
	if (S_ISDIR(cur->files->fds[fd]->f_inode->i_mode))
		return -EISDIR;

	return fs_pwrite(fd, offset, buf, count);
}

int sys_ioctl(int fd, int request, char *buf)
{
	int ret = fs_ioctl(fd, request, buf);

	if (TEST_LOG(TEST_LOG_TRACE) && request != TCGETS2 &&
	    request != 0x4b46 && request != 0x4b47 && request != 0x4b48 &&
	    request != 0x4b49)
		klog("ioctl(%d, %x, ...) = %d\n", fd, request, ret);

	return ret;
}

int sys_open(const char *_name, int flags, umode_t mode)
{
	char *name = name_get();
	int fd;
	if (!name)
		return -ENOMEM;

	resolve_path(_name, name);

	fd = fs_open(name, flags, mode);

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("open(%s, %x, %x) = %d\n", name, flags, mode, fd);

	name_put(name);
	return fd;
}

int sys_openat(int dirfd, const char *path, int flags, umode_t mode)
{
	char *name = name_get();
	if (!name)
		return -ENOMEM;
	int ret = syscall_resolve_at(dirfd, path, name);

	if (ret == 0)
		ret = fs_open(name, flags, mode);
	name_put(name);
	return ret;
}

int sys_close(unsigned fd)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("close(%d)\n", fd);

	return fs_close(fd);
}

int sys_close_range(unsigned first, unsigned last, unsigned flags)
{
	unsigned fd;
	if (flags)
		return -EINVAL;
	if (first > last || first >= MAX_FD)
		return first > last ? -EINVAL : 0;
	if (last >= MAX_FD)
		last = MAX_FD - 1;
	for (fd = first; fd <= last; fd++)
		fs_close((int)fd);
	return 0;
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
		klog("llseek(%d, %x, %x, %x, %d) = %d\n", fd, offset_high,
		     offset_low, result, whence, ret);

	return ret;
}

int sys_readv(int fildes, const struct iovec *iov, int iovcnt)
{
	int i;
	int ret, handled;
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
	if (cur->files->fds[fildes] == NULL)
		return -EBADF;

	fp = cur->files->fds[fildes];
	if (!fp || !fp->f_fop || !fp->f_fop->read)
		return -EBADF;
	if (S_ISDIR(fp->f_inode->i_mode))
		return -EISDIR;

	for (i = 0; i < iovcnt; i++)
		total_len += iov[i].iov_len;

	if (total_len == 0)
		return 0;

	ret = fs_readv_special(fildes, iov, iovcnt, &handled);
	if (handled)
		return ret;

	buf = malloc(total_len);
	if (!buf)
		return -ENOMEM;

	ret = fs_read(fildes, -1, buf, total_len);
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
	int ret, handled;
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
	if (cur->files->fds[fildes] == NULL)
		return -EBADF;

	fp = cur->files->fds[fildes];
	if (!fp || !fp->f_fop || !fp->f_fop->write)
		return -EBADF;
	if (S_ISDIR(fp->f_inode->i_mode))
		return -EISDIR;

	for (i = 0; i < iovcnt; i++)
		total_len += iov[i].iov_len;

	if (total_len == 0)
		return 0;
	ret = fs_writev_special(fildes, iov, iovcnt, &handled);
	if (handled)
		return ret;

	buf = malloc(total_len);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0)
			continue;
		memcpy(buf + copied, iov[i].iov_base, iov[i].iov_len);
		copied += iov[i].iov_len;
	}

	ret = fs_write(fildes, -1, buf, total_len);
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

	if (oldfd < 0 || newfd < 0)
		return -EBADF;
	if (oldfd >= MAX_FD || newfd >= MAX_FD)
		return -EBADF;

	return fs_dup2(oldfd, newfd);
}

int sys_dup3(int oldfd, int newfd, int flags)
{
	int ret = fs_dup3(oldfd, newfd, flags);

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("dup3(%d, %d, %x) = %d\n", oldfd, newfd, flags, ret);
	return ret;
}

int sys_pipe(int pipefd[2])
{
	int ret = fs_pipe(pipefd);

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("pipe(pipefd[%d,%d]) = %d\n", pipefd[0], pipefd[1], ret);

	return ret;
}

int sys_pipe2(int pipefd[2], int flags)
{
	int ret;
	task_struct *cur;

	if (flags & ~(O_NONBLOCK | O_CLOEXEC))
		return -EINVAL;
	ret = sys_pipe(pipefd);
	if (ret < 0)
		return ret;
	cur = CURRENT_TASK();
	cur->files->fds[pipefd[0]]->f_flag |= flags & O_NONBLOCK;
	cur->files->fds[pipefd[1]]->f_flag |= flags & O_NONBLOCK;
	if (flags & O_CLOEXEC) {
		fd_bitmap_set(cur->files->cloexec, pipefd[0]);
		fd_bitmap_set(cur->files->cloexec, pipefd[1]);
	}
	return 0;
}

static int flock_lazy_init(inode *in)
{
	if (!in)
		return -EINVAL;
	if (!in->i_flock_inited) {
		spinlock_init(&in->i_flock_lock);
		list_init(&in->i_flock_wait);
		in->i_flock_inited = 1;
	}
	return 0;
}

static int sys_fcntl_lock64(int fd, int cmd, struct flock64 *fl)
{
	struct flock64 local;
	int ret;
	if (!fl)
		return -EFAULT;
	local = *fl;
	ret = fs_posix_lock_fd(fd, cmd, &local);
	if (!ret && cmd == F_GETLK64)
		*fl = local;
	return ret;
}

static int sys_fcntl_lock32(int fd, int cmd, struct flock *fl)
{
	struct flock64 wide;
	int ret;
	if (!fl)
		return -EFAULT;
	wide = (struct flock64){ .l_type = fl->l_type,
				 .l_whence = fl->l_whence,
				 .l_start = fl->l_start,
				 .l_len = fl->l_len,
				 .l_pid = fl->l_pid };
	ret = sys_fcntl_lock64(fd, cmd + (F_GETLK64 - F_GETLK), &wide);
	if (!ret && cmd == F_GETLK) {
		if (wide.l_start > INT32_MAX || wide.l_len > INT32_MAX)
			return -EOVERFLOW;
		fl->l_type = wide.l_type;
		fl->l_whence = wide.l_whence;
		fl->l_start = wide.l_start;
		fl->l_len = wide.l_len;
		fl->l_pid = wide.l_pid;
	}
	return ret;
}

int sys_fcntl(int fd, int cmd, intptr_t arg)
{
	task_struct *cur = CURRENT_TASK();
	int ret = 0;

	if (fd < 0 || fd >= MAX_FD)
		return -EBADF;
	if (cur->files->fds[fd] == NULL)
		return -EBADF;

	switch (cmd) {
	case F_DUPFD:
		ret = fs_dup_from(fd, arg);
		break;
	case F_DUPFD_CLOEXEC:
		ret = fs_dup_from_flags(fd, arg, O_CLOEXEC);
		break;
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
		ret = sys_fcntl_lock32(fd, cmd, (struct flock *)(uintptr_t)arg);
		break;
	case F_GETFD:
		ret = fd_bitmap_test(cur->files->cloexec, fd) ? FD_CLOEXEC : 0;
		break;
	case F_SETFD:
		if (arg & FD_CLOEXEC)
			fd_bitmap_set(cur->files->cloexec, fd);
		else
			fd_bitmap_clear(cur->files->cloexec, fd);
		ret = 0;
		break;
	case F_GETFL:
		ret = cur->files->fds[fd]->f_flag;
		break;
	case F_SETFL:
		cur->files->fds[fd]->f_flag =
			(cur->files->fds[fd]->f_flag & O_ACCMODE) |
			(arg & ~(O_ACCMODE | O_CLOEXEC));
		ret = 0;
		break;
	case F_SETOWN:
		cur->files->fds[fd]->f_owner = arg;
		ret = 0;
		break;
	case F_GETOWN:
		ret = cur->files->fds[fd]->f_owner;
		break;
	case F_SETSIG:
		cur->files->fds[fd]->f_sigio = arg;
		ret = 0;
		break;
	case F_GETSIG:
		ret = cur->files->fds[fd]->f_sigio;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("fcntl(%d, %d, %d) = %d\n", fd, cmd, arg, ret);

	return ret;
}

int sys_fcntl64(int fd, int cmd, intptr_t arg)
{
	switch (cmd) {
	case F_GETLK64:
	case F_SETLK64:
	case F_SETLKW64:
		return sys_fcntl_lock64(fd, cmd,
					(struct flock64 *)(uintptr_t)arg);
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

	fp = current->files->fds[fd];

	if (count < sizeof(struct linux_dirent))
		return -22;
	if (!fp->f_fop || !fp->f_fop->read)
		return -1;

	n = fp->f_fop->read(fp, dirp, count, &fp->f_pos);
	if (n > 0)
		inotify_file_event(fp, IN_ACCESS);
	if (n < 0)
		return -1;
	return (size_t)n;
}

struct native_dirent {
	uint64_t ino, offset;
	uint16_t reclen;
	char name[];
};

static void dirent64_emit(void *buffer, const struct linux_dirent *source,
			  unsigned length, unsigned size, unsigned char type)
{
	struct linux_dirent64 *entry = buffer;
	entry->d_ino = source->d_ino;
	entry->d_off = source->d_off;
	entry->d_reclen = size;
	entry->d_type = type;
	memcpy(entry->d_name, source->d_name, length + 1);
}

static void native_dirent_emit(void *buffer, const struct linux_dirent *source,
			       unsigned length, unsigned size,
			       unsigned char type)
{
	struct native_dirent *entry = buffer;
	entry->ino = source->d_ino;
	entry->offset = source->d_off;
	entry->reclen = size;
	memcpy(entry->name, source->d_name, length + 1);
	((unsigned char *)buffer)[size - 1] = type;
}

static int getdents_convert(unsigned fd, void *output, unsigned count,
			    unsigned name_offset, unsigned trailer,
			    void (*emit)(void *, const struct linux_dirent *,
					 unsigned, unsigned, unsigned char))
{
	file *fp;
	char *buffer, *source;
	unsigned out = 0;
	loff_t position;
	int bytes;
	if (fd >= MAX_FD || !current->files->fds[fd])
		return -ENOENT;
	if (!output)
		return -EFAULT;
	if (count < name_offset + 2)
		return -EINVAL;
	buffer = malloc(count);
	if (!buffer)
		return -ENOMEM;
	fp = current->files->fds[fd];
	position = fp->f_pos;
	bytes = sys_getdents(fd, (void *)buffer, count);
	if (bytes < 0) {
		free(buffer);
		return bytes;
	}
	for (source = buffer; source < buffer + bytes;) {
		struct linux_dirent *entry = (void *)source;
		/* Byte-backed virtual directories can end a read within a record. */
		if (buffer + bytes - source < NAME_OFFSET() ||
		    entry->d_reclen > buffer + bytes - source) {
			fp->f_pos = position;
			break;
		}
		unsigned length = strlen(entry->d_name);
		unsigned size = (name_offset + length + trailer + 7) & ~7U;
		if (size > count - out) {
			/* Retain the first record that does not fit for the next call. */
			fp->f_pos = position;
			break;
		}
		memset((char *)output + out, 0, size);
		unsigned char type =
			fp->f_fop->dirent_type ?
				fp->f_fop->dirent_type(fp, entry->d_name) :
				0;
		emit((char *)output + out, entry, length, size, type);
		position = entry->d_off;
		source += entry->d_reclen;
		out += size;
	}
	free(buffer);
	return bytes && !out ? -EINVAL : (int)out;
}

int sys_getdents64(unsigned fd, struct linux_dirent64 *output, unsigned count)
{
	return getdents_convert(fd, output, count, NAME64_OFFSET(), 1,
				dirent64_emit);
}

int do_getdents_native(unsigned fd, void *output, unsigned count)
{
	return getdents_convert(fd, output, count,
				offset_of(struct native_dirent, name), 2,
				native_dirent_emit);
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

int sys_pselect6(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
		 const struct timespec *timeout, const void *sigmask_arg)
{
	struct {
		const sigset_t *mask;
		unsigned size;
	} *arg = (void *)sigmask_arg;
	struct timeval tv;
	const struct timeval *tvp = NULL;
	sigset_t mask;

	if (timeout) {
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
		    timeout->tv_nsec >= 1000000000)
			return -EINVAL;
		tv.tv_sec = timeout->tv_sec;
		tv.tv_usec = (timeout->tv_nsec + 999) / 1000;
		if (tv.tv_usec >= 1000000) {
			tv.tv_sec++;
			tv.tv_usec = 0;
		}
		tvp = &tv;
	}
	if (arg) {
		if (!arg->mask || arg->size != 8)
			return -EINVAL;
		mask = *(const sigset_t *)arg->mask;
		return do_select(nfds, readfds, writefds, exceptfds, tvp,
				 &mask);
	}
	return do_select(nfds, readfds, writefds, exceptfds, tvp, NULL);
}

int sys_poll(struct pollfd *fds, unsigned nfds, int timeout)
{
	if (TEST_LOG(TEST_LOG_TRACE))
		klog("poll(%x, %d, %d)\n", fds, nfds, timeout);

	return do_poll(fds, nfds, timeout);
}

int sys_ppoll(struct pollfd *fds, unsigned nfds, const struct timespec *timeout,
	      const sigset_t *sigmask, unsigned sigsetsize)
{
	if (sigmask && sigsetsize != 8)
		return -EINVAL;
	return do_ppoll(fds, nfds, timeout, sigmask);
}

/* 225: readahead — hint to preload file pages; MOS has no page cache, no-op */
int sys_readahead(int fd, unsigned offset_hi, unsigned offset_lo,
		  unsigned count)
{
	(void)fd;
	(void)offset_hi;
	(void)offset_lo;
	(void)count;
	return 0;
}

ssize_t sys_sendfile(int out_fd, int in_fd, int32_t *offset, size_t count)
{
	int32_t old_position = 0;
	if (offset && (ps_read_process_memory(current, offset, &old_position,
					      sizeof(old_position)) < 0 ||
		       ps_write_process_memory(current, offset, &old_position,
					       sizeof(old_position)) < 0))
		return -EFAULT;
	if (offset && old_position < 0)
		return -EINVAL;
	if (offset && count > 0x7fffffffU - (unsigned)old_position)
		count = 0x7fffffffU - (unsigned)old_position;
	int64_t position = old_position;
	ssize_t result =
		fs_sendfile(out_fd, in_fd, offset ? &position : NULL, count);
	if (offset && result >= 0) {
		old_position = position;
		if (ps_write_process_memory(current, offset, &old_position,
					    sizeof(old_position)) < 0)
			return -EFAULT;
	}
	return result;
}

ssize_t sys_sendfile64(int out_fd, int in_fd, int64_t *offset, size_t count)
{
	int64_t position = 0;
	if (offset && (ps_read_process_memory(current, offset, &position,
					      sizeof(position)) < 0 ||
		       ps_write_process_memory(current, offset, &position,
					       sizeof(position)) < 0))
		return -EFAULT;
	ssize_t result =
		fs_sendfile(out_fd, in_fd, offset ? &position : NULL, count);
	if (offset && result >= 0 &&
	    ps_write_process_memory(current, offset, &position,
				    sizeof(position)) < 0)
		return -EFAULT;
	return result;
}
