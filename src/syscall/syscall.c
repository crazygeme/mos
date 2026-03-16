#include <mm/mm.h>
#include <int/int.h>
#include <ps/ps.h>
#include <elf/exec.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <fs/fs.h>
#include <fs/select.h>
#include <fs/fcntl.h>
#include <unistd.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include <ext4.h>

struct utimbuf {
	unsigned actime; /* access time */
	unsigned modtime; /* modification time */
};

struct mmap_arg_struct32 {
	unsigned int addr;
	unsigned int len;
	unsigned int prot;
	unsigned int flags;
	int fd;
	unsigned int offset;
};

struct iovec {
	char *iov_base; /* Base address. */
	unsigned iov_len; /* Length. */
};

enum {
	UNAME26 = 0x0020000,
	ADDR_NO_RANDOMIZE = 0x0040000, /* disable randomization of VA space */
	FDPIC_FUNCPTRS =
		0x0080000, /* userspace function ptrs point to descriptors
                                    * (signal handling)
                                    */
	MMAP_PAGE_ZERO = 0x0100000,
	ADDR_COMPAT_LAYOUT = 0x0200000,
	READ_IMPLIES_EXEC = 0x0400000,
	ADDR_LIMIT_32BIT = 0x0800000,
	SHORT_INODE = 0x1000000,
	WHOLE_SECONDS = 0x2000000,
	STICKY_TIMEOUTS = 0x4000000,
	ADDR_LIMIT_3GB = 0x8000000,
};

/*
 * Security-relevant compatibility flags that must be
 * cleared upon setuid or setgid exec:
 */
#define PER_CLEAR_ON_SETID                                            \
	(READ_IMPLIES_EXEC | ADDR_NO_RANDOMIZE | ADDR_COMPAT_LAYOUT | \
	 MMAP_PAGE_ZERO)

enum {
	PER_LINUX = 0x0000,
	PER_LINUX_32BIT = 0x0000 | ADDR_LIMIT_32BIT,
	PER_LINUX_FDPIC = 0x0000 | FDPIC_FUNCPTRS,
	PER_SVR4 = 0x0001 | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
	PER_SVR3 = 0x0002 | STICKY_TIMEOUTS | SHORT_INODE,
	PER_SCOSVR3 = 0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS | SHORT_INODE,
	PER_OSR5 = 0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS,
	PER_WYSEV386 = 0x0004 | STICKY_TIMEOUTS | SHORT_INODE,
	PER_ISCR4 = 0x0005 | STICKY_TIMEOUTS,
	PER_BSD = 0x0006,
	PER_SUNOS = 0x0006 | STICKY_TIMEOUTS,
	PER_XENIX = 0x0007 | STICKY_TIMEOUTS | SHORT_INODE,
	PER_LINUX32 = 0x0008,
	PER_LINUX32_3GB = 0x0008 | ADDR_LIMIT_3GB,
	PER_IRIX32 = 0x0009 | STICKY_TIMEOUTS, /* IRIX5 32-bit */
	PER_IRIXN32 = 0x000a | STICKY_TIMEOUTS, /* IRIX6 new 32-bit */
	PER_IRIX64 = 0x000b | STICKY_TIMEOUTS, /* IRIX6 64-bit */
	PER_RISCOS = 0x000c,
	PER_SOLARIS = 0x000d | STICKY_TIMEOUTS,
	PER_UW7 = 0x000e | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
	PER_OSF4 = 0x000f, /* OSF/1 v4 */
	PER_HPUX = 0x0010,
	PER_MASK = 0x00ff,
};

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2);
static int sys_read(int fd, char *buf, unsigned len);
static int sys_write(int fd, char *buf, unsigned len);
static int sys_getpid();
static int sys_mount(char *dev, char *dir_name, char *type, unsigned flag,
		     void *data);
static int sys_uname(struct utsname *utname);
static int sys_open(const char *name, int flags, char *mode);
static int sys_close(unsigned fd);
static int sys_sched_yield();
static int sys_brk(unsigned top);
static int sys_chdir(const char *path);
static int sys_ioctl(int d, int request, char *buf);
static int sys_creat(const char *path, unsigned mode);
static int sys_mkdir(const char *path, unsigned mode);
static int sys_rmdir(const char *path);
static int sys_readdir(unsigned fd, struct linux_dirent *dirp, unsigned count);
static int sys_reboot(unsigned cmd);
static int sys_getuid();
static int sys_getgid();
static int sys_geteuid();
static int sys_getegid();
static int sys_setreuid(unsigned ruid, unsigned euid);
static int sys_setregid(unsigned pid, unsigned pgid);
static int sys_mmap(struct mmap_arg_struct32 *arg);
static int sys_mprotect(void *addr, unsigned len, int prot);
static int sys_readv(int fildes, const struct iovec *iov, int iovcnt);
static int sys_writev(int fildes, const struct iovec *iov, int iovcnt);
static long sys_personality(unsigned int personality);
static int sys_fcntl(int fd, int cmd, int arg);
static int sys_getdents(unsigned int fd, struct linux_dirent *dirp,
			unsigned int count);
static int sys_oldstat(const char *filename, struct oldstat *buf);
static int sys_stat(const char *pathname, struct stat *buf);
static int sys_lstat(const char *path, struct stat *s);
static int sys_fstat(int fd, struct stat *s);
static int sys_stat64(const char *pathname, struct stat64 *s);
static int sys_lstat64(const char *path, struct stat64 *s);
static int sys_fstat64(int fd, struct stat64 *s);
static int sys_fsync(int fd);
static int sys_munmap(void *addr, unsigned length);
static int sys_getppid();
static int sys_getpgrp(unsigned pid);
static int sys_setpgid(unsigned pid, unsigned pgid);
static int sys_wait4(int pid, int *status, int options, void *rusage);
static int sys_socketcall(int call, unsigned long *args);
static int sys_sigaction(int sig, void *act, void *oact);
static int sys_sigprocmask(int how, void *set, void *oset);
static int sys_pause();
static int sys_utime(const char *filename, const struct utimbuf *times);
static int sys_pipe(int pipefd[2]);
static int sys_dup(int oldfd);
static int sys_dup2(int oldfd, int newfd);
static int sys_getrlimit(int resource, void *limit);
static int sys_kill(unsigned pid, int sig);
static int sys_unlink(const char *pathname);
static int sys_time(unsigned *t);
int resolve_path(const char *old, char *new);
static int sys_access(const char *path, int mode);
static int do_stat(const char *func, const char *_name, struct stat *buf,
		   int follow_link);
static int sys_lseek(int fd, int offset, int whence);
static int sys_llseek(int fd, unsigned offset_high, unsigned offset_low,
		      uint64_t *result, unsigned int whence);
static int sys_select(int nfds, fd_set *readfds, fd_set *writefds,
		      fd_set *exceptfds, const struct timespec *timeout);
static int sys_newselect(int nfds, fd_set *readfds, fd_set *writefds,
			 fd_set *exceptfds, const struct timespec *timeout,
			 void *sigmask);
static int sys_link(const char *path1, const char *path2);
static int sys_symlink(const char *path1, const char *path2);
static int sys_chmod(const char *pathname, uint32_t mode);
static int sys_fchmod(int fd, uint32_t mode);
static int sys_rename(const char *oldpath, const char *newpath);
static int sys_umask(unsigned mask);
static int sys_gettimeofday(struct timeval *tv, struct timezone *tz);
static int sys_nanosleep(const struct timespec *req, struct timespec *rem);
static unsigned sys_alarm(unsigned seconds);

typedef int (*syscall_fn)(unsigned ebx, unsigned ecx, unsigned edx,
			  unsigned esi, unsigned edi, unsigned ebp);

static unsigned call_table[NR_syscalls] = {
	test_call,
	sys_exit,
	sys_fork,
	sys_read,
	sys_write,
	sys_open, // 1  ~ 5
	sys_close,
	sys_waitpid,
	sys_creat,
	sys_link,
	sys_unlink, // 6  ~ 10
	sys_execve,
	sys_chdir,
	sys_time,
	0,
	sys_chmod, // 11 ~ 15
	0,
	0,
	0,
	sys_lseek,
	sys_getpid, // 16 ~ 20
	sys_mount,
	sys_oldstat,
	0,
	sys_getuid,
	0, // 21 ~ 25
	0,
	sys_alarm,
	0,
	sys_pause,
	sys_utime, // 26 ~ 30
	0,
	0,
	sys_access,
	0,
	0, // 31 ~ 35
	0,
	sys_kill,
	sys_rename,
	sys_mkdir,
	sys_rmdir, // 36 ~ 40
	sys_dup,
	sys_pipe,
	0,
	0,
	sys_brk, // 41 ~ 45
	0,
	sys_getgid,
	0,
	sys_geteuid,
	sys_getegid, // 46 ~ 50
	0,
	0,
	0,
	sys_ioctl,
	sys_fcntl, // 51 ~ 55
	0,
	sys_setpgid,
	0,
	0,
	sys_umask, // 56 ~ 60
	0,
	0,
	sys_dup2,
	sys_getppid,
	sys_getpgrp, // 61 ~ 65
	0,
	0,
	0,
	0,
	sys_setreuid, // 66 ~ 70
	sys_setregid,
	0,
	0,
	0,
	0, // 71 ~ 75
	sys_getrlimit,
	0,
	sys_gettimeofday,
	0,
	0, // 76 ~ 80
	0,
	sys_select,
	sys_symlink,
	0,
	0, // 81 ~ 85
	0,
	0,
	sys_reboot,
	sys_readdir,
	sys_mmap, // 86 ~ 90
	sys_munmap,
	0,
	0,
	sys_fchmod,
	0, // 91 ~ 95
	0,
	0,
	0,
	0,
	0, // 96 ~ 100
	0,
	sys_socketcall,
	0,
	0,
	0, // 101 ~ 105
	sys_stat,
	sys_lstat,
	sys_fstat,
	0,
	0, // 106 ~ 110
	0,
	0,
	0,
	sys_wait4,
	0, // 111 ~ 115
	0,
	0,
	sys_fsync,
	0,
	0, // 116 ~ 120
	0,
	sys_uname,
	0,
	0,
	sys_mprotect, // 121 ~ 125
	0,
	0,
	0,
	0,
	0, // 126 ~ 130
	0,
	0,
	0,
	0,
	0, // 131 ~ 135
	sys_personality,
	0,
	0,
	0,
	sys_llseek, // 136 ~ 140
	sys_getdents,
	sys_newselect,
	0,
	0,
	sys_readv, // 141 ~ 145
	sys_writev,
	0,
	0,
	0,
	0, // 146 ~ 150
	0,
	0,
	0,
	0,
	0, // 151 ~ 155
	0,
	0,
	sys_sched_yield,
	0,
	0, // 156 ~ 160
	0,
	sys_nanosleep,
	0,
	0,
	0, // 161 ~ 165
	0,
	0,
	0,
	0,
	0, // 165 ~ 170
	0,
	0,
	0,
	sys_sigaction,
	sys_sigprocmask, // 171 ~ 175
	0,
	0,
	0,
	0,
	0, // 175 ~ 180
	0,
	0,
	sys_getcwd,
	0,
	0, // 181 ~ 185
	0,
	0,
	0,
	0,
	sys_vfork, // 185 ~ 190
	0,
	0,
	0,
	0,
	sys_stat64, // 191 ~ 195
	sys_lstat64,
	sys_fstat64,
	0 // 196 ~ 198
};

static int unhandled_syscall(unsigned callno)
{
	if (TestControl.verbos)
		klog("unhandled syscall %d\n", callno);
	return -ENOSYS;
}

static void syscall_process(intr_frame *frame)
{
	task_struct *cur = CURRENT_TASK();

	syscall_fn fn = call_table[frame->eax];
	int ret = 0;
	if (!fn) {
		ret = unhandled_syscall(frame->eax);
	} else {
		ret = (unsigned)fn(frame->ebx, frame->ecx, frame->edx,
				   frame->esi, frame->edi, frame->ebp);
	}

	frame->eax = ret;
	return;
}

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2)
{
	printk("test call: arg0 %x, arg1 %x, arg2 %x\n", arg0, arg1, arg2);
	return 0;
}

static int sys_read(int fd, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();
	int i = 0, pri_len = 0;
	int ret = 0;
	if (fd < 0 || fd >= MAX_FD)
		return -1;

	if (cur->fds[fd].used == 0)
		return -1;

	if (S_ISDIR(cur->fds[fd].fp->f_inode->i_mode))
		return -1;

	if (TestControl.verbos)
		klog("read(%d, %x, %d)\n", fd, buf, len);

	ret = fs_read(fd, -1, buf, len);

	return ret;
}

static int sys_write(int fd, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();
	if (TestControl.verbos)
		klog("write(%d, %x, %d)\n", fd, buf, len);

	if (fd < 0 || fd >= MAX_FD) {
		return -1;
	}

	if (cur->fds[fd].used == 0) {
		return -1;
	}

	if (S_ISDIR(cur->fds[fd].fp->f_inode->i_mode)) {
		return -1;
	}

	return fs_write(fd, -1, buf, len);
}

static int sys_ioctl(int fd, int request, char *buf)
{
	task_struct *cur = CURRENT_TASK();

	int ret = fs_ioctl(fd, request, buf);
	if (TestControl.verbos)
		klog("ioctl(%d, %x, ...) = %d\n", fd, request, ret);

	return ret;
}

static int sys_getpid()
{
	task_struct *cur = CURRENT_TASK();
	if (TestControl.verbos)
		klog("getpid() = %d\n", cur->psid);

	return cur->psid;
}

static int sys_mount(char *dev, char *dir_name, char *type, unsigned flag,
		     void *data)
{
	if (TestControl.verbos)
		klog("mount(%s, %s, %s, %d, %x)\n", dev, dir_name, type, flag,
		     data);

	if (strcmp(dev, "/proc") == 0)
		return 0;

	return -1;
}

static int sys_uname(struct utsname *utname)
{
	if (TestControl.verbos)
		klog("uname\n");

	strcpy(utname->machine, "i386");
	strcpy(utname->nodename, "qemu-enum");
	strcpy(utname->release, "0.91-generic");
	strcpy(utname->sysname, "Mos");
	strcpy(utname->version, "Mos Wed Mar 11 15:00:00 UTC 2026");
	strcpy(utname->domain, "Ender");
	return 1;
}

static int sys_sched_yield()
{
	if (TestControl.verbos)
		klog("yield\n");

	task_sched();
	return 0;
}

static int sys_open(const char *_name, int flags, char *mode)
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

static int sys_close(unsigned fd)
{
	if (TestControl.verbos)
		klog("close(%d)\n", fd);

	return fs_close(fd);
}

static int sys_brk(unsigned _top)
{
	task_struct *task = CURRENT_TASK();
	unsigned size = 0;
	unsigned pages = 0;
	unsigned ret, top;
	top = _top;
	if (task->user.heap_top == USER_HEAP_BEGIN) {
		do_mmap(task->user.heap_top, PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, -1, 0);
		task->user.heap_top += PAGE_SIZE;
	}

	if (top == 0) {
		ret = task->user.heap_top;
		goto done;
	} else if (top >= USER_HEAP_END) {
		ret = task->user.heap_top;
		goto done;
	} else if (top > task->user.heap_top) {
		int i = 0;
		size = top - task->user.heap_top;
		pages = (size - 1) / PAGE_SIZE + 1;
		do_mmap(task->user.heap_top, PAGE_SIZE * pages,
			PROT_READ | PROT_WRITE, 0, -1, 0);
		top = task->user.heap_top + pages * PAGE_SIZE;
		task->user.heap_top = top;
		ret = top;
		goto done;
	} else {
		int i = 0;

		if (top < USER_HEAP_BEGIN)
			top = USER_HEAP_BEGIN;

		size = task->user.heap_top - top;
		pages = (size - 1) / PAGE_SIZE + 1;
		do_munmap(top & PAGE_SIZE_MASK, pages * PAGE_SIZE);
		task->user.heap_top = top;
		ret = top;
		goto done;
	}
done:
	if (TestControl.verbos)
		klog("brk(%x) = %x\n", _top, ret);

	return ret;
}

static int sys_chdir(const char *path)
{
	task_struct *cur = CURRENT_TASK();
	char *cwd = name_get();
	const char *p;
	struct stat s;
	int ret = 0;

	if (!path || !*path) {
		ret = -ENOENT;
		goto done;
	}

	if (TestControl.verbos)
		klog("chdir(%s)\n", path);

	/* Seed cwd from root or from the current directory */
	if (*path == '/') {
		cwd[0] = '/';
		cwd[1] = '\0';
		p = path + 1;
	} else {
		int len;
		strcpy(cwd, cur->cwd);
		len = strlen(cwd);
		if (!len || cwd[len - 1] != '/')
			strcat(cwd, "/");
		p = path;
	}

	/* Walk each path component */
	while (*p) {
		const char *end;
		int comp_len, len;

		while (*p == '/') /* skip consecutive slashes */
			p++;
		if (!*p)
			break;

		end = p;
		while (*end && *end != '/')
			end++;
		comp_len = end - p;

		if (comp_len == 1 && p[0] == '.') {
			/* '.' — stay */
		} else if (comp_len == 2 && p[0] == '.' && p[1] == '.') {
			/* '..' — ascend one level */
			len = strlen(cwd);
			if (len > 1 && cwd[len - 1] == '/')
				cwd[--len] = '\0';
			char *sep = strrchr(cwd, '/');
			if (sep == cwd)
				cwd[1] = '\0'; /* already at root */
			else if (sep)
				*sep = '\0';
			len = strlen(cwd);
			if (!len || cwd[len - 1] != '/')
				strcat(cwd, "/");
		} else {
			/* Regular component — append, then verify */
			len = strlen(cwd);
			memcpy(cwd + len, p, comp_len);
			cwd[len + comp_len] = '/';
			cwd[len + comp_len + 1] = '\0';
			if (do_stat(NULL, cwd, &s, 1) != EOK) {
				ret = -ENOENT;
				goto done;
			}
			if (!S_ISDIR(s.st_mode)) {
				ret = -ENOTDIR;
				goto done;
			}
		}
		p = end;
	}

	strcpy(cur->cwd, cwd);
done:
	name_put(cwd);
	return ret;
}

static int sys_creat(const char *path, unsigned mode)
{
	if (TestControl.verbos)
		klog("creat(%s, %d)\n", path, mode);

	return -1;
}

static int sys_mkdir(const char *path, unsigned mode)
{
	char *name = name_get();
	int ret;
	resolve_path(path, name);
	ret = ext4_dir_mk(name);
	if (TestControl.verbos)
		klog("mkdir(%s, %d)\n", path, mode);

	name_put(name);
	return ret;
}

static int sys_rmdir(const char *path)
{
	char *name = name_get();
	int ret;
	resolve_path(path, name);
	ret = ext4_dir_rm(name);
	if (TestControl.verbos)
		klog("rmdir(%s)\n", name);

	name_put(name);
	return ret;
}

static int sys_reboot(unsigned cmd)
{
	switch (cmd) {
	case MOS_REBOOT_CMD_RESTART:
		reboot();
		break;
	case MOS_REBOOT_CMD_POWER_OFF:
		shutdown();
		break;
	default:
		break;
	}
	return 0;
}

static int sys_getuid()
{
	if (TestControl.verbos)
		klog("getuid\n");

	return 0;
}

static int sys_getgid()
{
	if (TestControl.verbos)
		klog("getgid\n");

	return 0;
}

static int sys_geteuid()
{
	if (TestControl.verbos)
		klog("geteuid\n");

	return 0;
}

static int sys_getegid()
{
	if (TestControl.verbos)
		klog("getegid\n");

	return 0;
}
static int debug = 0;

static int sys_mmap(struct mmap_arg_struct32 *arg)
{
	int vir = 0;

	if (arg->len > (10 * 1024 * 1024)) {
		return -1;
	}

	vir = do_mmap(arg->addr, arg->len, arg->prot, arg->flags, arg->fd,
		      arg->offset);

	if (TestControl.verbos)
		klog("mmap: fd %d, addr %x, offset %x, len %x at addr %x\n",
		     arg->fd, arg->addr, arg->offset, arg->len, vir);

	return vir;
}

static int sys_munmap(void *addr, unsigned length)
{
	if (TestControl.verbos)
		klog("munmap (%x, %x)\n", addr, length);

	return do_munmap(addr, length);
}

static int sys_mprotect(void *addr, unsigned len, int prot)
{
	if (TestControl.verbos)
		klog("mprotect: addr %x, len %x, prot %x\n", addr, len, prot);

	return 0;
}

static int sys_readv(int fildes, const struct iovec *iov, int iovcnt)
{
	int i = 0;
	unsigned total = 0;
	for (i = 0; i < iovcnt; i++) {
		total += iov[i].iov_len;
		memset(iov[i].iov_base, 0, iov[i].iov_len);
	}

	return total;
}

static int sys_writev(int fildes, const struct iovec *iov, int iovcnt)
{
	if (TestControl.verbos)
		klog("writev: fd %d\n", fildes);

	int i = 0;
	unsigned total = 0;
	for (i = 0; i < iovcnt; i++) {
		total += sys_write(fildes, iov[i].iov_base, iov[i].iov_len);
	}
	return total;
}

static long sys_personality(unsigned int personality)
{
	if (TestControl.verbos)
		klog("personality\n");

	return PER_LINUX32_3GB;
}

static int sys_fcntl(int fd, int cmd, int arg)
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
		// case F_DUPFD_CLOEXEC: // no in Linux ??
	case F_GETFD:
		if (cur->fds[fd].flag & O_CLOEXEC)
			ret = FD_CLOEXEC;
		else
			ret = 0;
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
	return ret;
}

static int sys_getdents(unsigned int fd, struct linux_dirent *dirp,
			unsigned int count)
{
	int ret = 0;
	struct stat s;
	file *fp;
	ext4_dir *dir;

	if (TestControl.verbos)
		klog("getdents(%d, %x, %d)\n", fd, dirp, count);

	if (fd < 0 || fd >= MAX_FD) {
		return -ENOENT;
	}

	if (fs_fstat(fd, &s) != EOK)
		return -ENOENT;

	if (!S_ISDIR(s.st_mode))
		return -EISDIR;

	fp = CURRENT_TASK()->fds[fd].fp;

	if (count < sizeof(struct linux_dirent)) {
		return -22;
	}

	if (!fp->f_fop || !fp->f_fop->read)
		return -1;
	ssize_t n = fp->f_fop->read(fp, dirp, count, &fp->f_pos);
	if (n < 0)
		return -1;
	return (size_t)n;
}

static int sys_fsync(int fd)
{
	if (TestControl.verbos)
		klog("sys_fsync(%d)\n", fd);

	return fs_sync(fd);
}

static int format_modes(unsigned mode, char *str)
{
	memset(str, '-', 11);
	str[0] = '-';
	if (S_ISDIR(mode))
		str[0] = 'd';
	else if (S_ISCHR(mode))
		str[0] = 'c';
	else if (S_ISLNK(mode))
		str[0] = 'l';

	if (mode & S_IRUSR)
		str[1] = 'r';

	if (mode & S_IWUSR)
		str[2] = 'w';

	if (mode & S_IXUSR)
		str[3] = 'x';

	if (mode & S_IRGRP)
		str[4] = 'r';

	if (mode & S_IWGRP)
		str[5] = 'w';

	if (mode & S_IXGRP)
		str[6] = 'x';

	if (mode & S_IROTH)
		str[7] = 'r';

	if (mode & S_IWOTH)
		str[8] = 'w';

	if (mode & S_IXOTH)
		str[9] = 'x';

	str[10] = '\0';

	return 0;
}

static int sys_fstat(int fd, struct stat *buf)
{
	int ret = fs_fstat(fd, buf);
	if (TestControl.verbos) {
		char modes[11];
		format_modes(buf->st_mode, modes);
		klog("fstat(%d, %x) = %d, %s, size=%d, blocks = %d, ino = %d, rdev = %d, dev = %d, nlink = %d\n",
		     fd, buf, ret, modes, buf->st_size, buf->st_blocks,
		     buf->st_ino, buf->st_rdev, buf->st_dev, buf->st_nlink);
	}

	return ret;
}

static int sys_fstat64(int fd, struct stat64 *buf)
{
	task_struct *cur = CURRENT_TASK();
	struct stat s32;

	if (fd < 0 || fd >= MAX_FD)
		return -1;

	int ret = fs_fstat(fd, &s32);
	buf->st_dev = s32.st_dev;
	buf->st_ino = s32.st_ino;
	buf->st_mode = s32.st_mode;
	buf->st_nlink = s32.st_nlink;
	buf->st_uid = s32.st_uid;
	buf->st_gid = s32.st_gid;
	buf->st_rdev = s32.st_rdev;
	buf->st_size = s32.st_size;
	buf->st_blksize = s32.st_blksize;
	buf->st_blocks = s32.st_blocks;
	buf->st_atime = s32.st_atime;
	buf->st_mtime = s32.st_mtime;
	buf->st_ctime = s32.st_ctime;

	if (TestControl.verbos) {
		char modes[11];
		format_modes(buf->st_mode, modes);
		klog("fstat(%d, %x) = %d, %s, size=%d, blocks = %d, ino = %d, rdev = %d, dev = %d, nlink = %d\n",
		     fd, buf, ret, modes, buf->st_size, buf->st_blocks,
		     buf->st_ino, buf->st_rdev, buf->st_dev, buf->st_nlink);
	}

	return ret;
}

static int sys_lstat64(const char *path, struct stat64 *s)
{
	struct stat s32;
	char *name = name_get();
	int ret;
	resolve_path(path, name);
	ret = do_stat("lstat64", name, &s32, 0);
	memset(s, 0, sizeof(*s));
	s->st_dev = s32.st_dev;
	s->__pad0 = 0;
	s->st_ino = s32.st_ino;
	s->st_mode = s32.st_mode;
	s->st_nlink = s32.st_nlink;
	s->st_uid = s32.st_uid;
	s->st_gid = s32.st_gid;
	s->st_rdev = s32.st_rdev;
	s->st_size = s32.st_size;
	s->st_blksize = s32.st_blksize;
	s->st_blocks = s32.st_blocks;
	s->st_atime = s32.st_atime;
	s->st_mtime = s32.st_mtime;
	s->st_ctime = s32.st_ctime;
	name_put(name);
	return ret;
}

static int sys_stat64(const char *path, struct stat64 *s)
{
	struct stat s32;
	char *name = name_get();
	int ret;
	resolve_path(path, name);
	ret = do_stat("stat64", name, &s32, 1);
	memset(s, 0, sizeof(*s));
	s->st_dev = s32.st_dev;
	s->__pad0 = 0;
	s->st_ino = s32.st_ino;
	s->st_mode = s32.st_mode;
	s->st_nlink = s32.st_nlink;
	s->st_uid = s32.st_uid;
	s->st_gid = s32.st_gid;
	s->st_rdev = s32.st_rdev;
	s->st_size = s32.st_size;
	s->st_blksize = s32.st_blksize;
	s->st_blocks = s32.st_blocks;
	s->st_atime = s32.st_atime;
	s->st_mtime = s32.st_mtime;
	s->st_ctime = s32.st_ctime;
	name_put(name);
	return ret;
}

static int sys_getppid()
{
	task_struct *cur = CURRENT_TASK();
	if (TestControl.verbos)
		klog("getppid\n");

	return cur->parent->psid;
}

static int sys_getpgrp(unsigned pid)
{
	if (TestControl.verbos)
		klog("getpgrp\n");

	// FIXME
	return 0;
}

static int sys_setpgid(unsigned pid, unsigned pgid)
{
	if (TestControl.verbos)
		klog("setgpid\n");

	// FIXME
	if (pid == 0) {
		task_struct *cur = CURRENT_TASK();
		cur->group_id = pgid;
	}
	return 0;
}

static int sys_wait4(int pid, int *status, int options, void *rusage)
{
	if (pid == -1) {
		return do_waitpid(0, status, options, rusage);
	} else if (pid < -1) {
		// FIXME
		// wait on group id
		return 0;
	} else {
		return do_waitpid(pid, status, options, rusage);
	}
}

static int sys_socketcall(int call, unsigned long *args)
{
	// FIXME
	// no socket at all right now
	if (TestControl.verbos)
		klog("socketcall(%d, %x)\n", call, args);

	return -1;
}

static int sys_sigaction(int sig, void *act, void *oact)
{
	// FIXME
	// no signal at all
	return -1;
}

static int sys_sigprocmask(int how, void *set, void *oset)
{
	// FIXME
	// no signal
	return -1;
}

static int sys_pause()
{
	if (TestControl.verbos)
		klog("pause\n");

	PAUSE();
	return 0;
}

static int sys_utime(const char *filename, const struct utimbuf *times)
{
	if (TestControl.verbos)
		klog("utime\n");

	return 0;
}

static unsigned sys_alarm(unsigned seconds)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long long now = time_now_ms();
	unsigned remaining = 0;

	if (cur->alarm_expire_ms > now)
		remaining =
			(unsigned)((cur->alarm_expire_ms - now + 999) / 1000);

	cur->alarm_expire_ms =
		seconds ? (now + (unsigned long long)seconds * 1000) : 0;

	if (TestControl.verbos)
		klog("alarm(%u) = %u\n", seconds, remaining);

	return remaining;
}

static int sys_pipe(int pipefd[2])
{
	int ret = fs_pipe(pipefd);

	if (TestControl.verbos)
		klog("pipe(pipefd[%d,%d]) = %d\n", pipefd[0], pipefd[1], ret);

	return ret;
}

static int sys_dup2(int oldfd, int newfd)
{
	int ret = -1;
	if (TestControl.verbos)
		klog("dup2(%d, %d)\n", oldfd, newfd);

	if (oldfd == -1 || newfd == -1)
		return -1;

	if (oldfd >= MAX_FD || newfd >= MAX_FD)
		return -1;

	if (oldfd == newfd)
		return -1;

	ret = fs_dup2(oldfd, newfd);

	return ret;
}

static int sys_dup(int oldfd)
{
	int ret = -1;

	if (oldfd == -1 || oldfd >= MAX_FD)
		return -1;

	ret = fs_dup(oldfd);

	if (TestControl.verbos)
		klog("dup(%d) = %d\n", oldfd, ret);

	return ret;
}

static int sys_getrlimit(int resource, void *limit)
{
	if (TestControl.verbos)
		klog("getrlimit\n");

	return -1;
}

static int sys_kill(unsigned pid, int sig)
{
	if (TestControl.verbos)
		klog("kill\n");

	return -1;
}

static int sys_unlink(const char *_name)
{
	char *name = name_get();
	struct stat s;
	int ret = -1;

	resolve_path(_name, name);
	ret = do_stat(NULL, name, &s, 0);
	if (ret != EOK)
		goto done;

	if (S_ISDIR(s.st_mode)) {
		ret = -EISDIR;
		goto done;
	}

	ret = ext4_fremove(name);
done:
	name_put(name);
	if (TestControl.verbos)
		klog("unlink(%s) = %d\n", name, ret);

	return ret;
}

static int sys_time(unsigned *t)
{
	if (!t)
		return -1;

	*t = (unsigned)time_now_ms();
	return 0;
}

static int sys_readdir(unsigned fd, struct linux_dirent *dirp, unsigned count)
{
	int ret = -1;
	if (TestControl.verbos)
		klog("readdir(%d, %x, %d) = %d\n", fd, dirp, count, ret);

	return sys_getdents(fd, dirp, count);
}

static int do_stat(const char *func, const char *name, struct stat *buf,
		   int follow_link)
{
	int ret = -ENOENT;
	file *fp = NULL;
	char modes[11];
	fp = fs_open_file(name, 0, "r", follow_link);
	if (fp == NULL)
		goto done;
	if (!fp->f_inode || !fp->f_inode->i_op || !fp->f_inode->i_op->getattr)
		goto done;
	ret = fp->f_inode->i_op->getattr(fp->f_inode, buf);
	if (TestControl.verbos && func) {
		format_modes(buf->st_mode, modes);
		klog("%s(%s, %x) = %d, %s, size=%d, blocks = %d, ino = %d, rdev = %d, dev = %d, nlink = %d\n",
		     func, name, buf, ret, modes, buf->st_size, buf->st_blocks,
		     buf->st_ino, buf->st_rdev, buf->st_dev, buf->st_nlink);
	}

done:
	if (fp) {
		fs_put_file(fp);
	}
	return ret;
}

static int sys_oldstat(const char *filename, struct oldstat *buf)
{
	struct stat s;
	int ret = sys_stat(filename, &s);
	if (ret != EOK)
		return ret;

	buf->st_dev = s.st_dev;
	buf->st_ino = s.st_ino;
	buf->st_mode = s.st_mode;
	buf->st_nlink = s.st_nlink;
	buf->st_uid = s.st_uid;
	buf->st_gid = s.st_gid;
	buf->st_rdev = s.st_rdev;
	buf->st_size = s.st_size;
	buf->st_atime = s.st_atime;
	buf->st_mtime = s.st_mtime;
	buf->st_ctime = s.st_ctime;

	return 0;
}

static int sys_stat(const char *_name, struct stat *buf)
{
	char *name = name_get();
	int ret;
	resolve_path(_name, name);
	ret = do_stat("stat", name, buf, 1);
	name_put(name);
	return ret;
}

static int sys_lstat(const char *_name, struct stat *buf)
{
	char *name = name_get();
	int ret;
	resolve_path(_name, name);
	ret = do_stat("lstat", name, buf, 0);
	name_put(name);
	return ret;
}

static int sys_access(const char *path, int mode)
{
	// FIXME: no user currently
	struct stat s;
	char *name = name_get();
	int ret = -EACCES;

	resolve_path(path, name);
	ret = do_stat(NULL, name, &s, 1);

	if (ret != EOK) {
		ret = -ENOENT;
		goto done;
	}

	if (mode == F_OK)
		goto done;

	ret = EOK;
	if (mode & R_OK)
		ret |= ((s.st_mode & S_IRUSR) == S_IRUSR) ? EOK : (-EACCES);

	if (mode & W_OK)
		ret |= ((s.st_mode & S_IWUSR) == S_IWUSR) ? EOK : (-EACCES);

	if (mode & X_OK)
		ret |= ((s.st_mode & S_IXUSR) == S_IXUSR) ? EOK : (-EACCES);

done:
	name_put(name);
	if (TestControl.verbos)
		klog("access(%s, %x) = %d\n", path, mode, ret);

	return ret;
}

static int sys_lseek(int fd, int offset, int whence)
{
	int ret;
	ret = fs_seek(fd, offset, whence);
	if (TestControl.verbos)
		klog("lseek(%d, %d, %d) = %d\n", fd, offset, whence, ret);

	return ret;
}

static int sys_llseek(int fd, unsigned offset_high, unsigned offset_low,
		      uint64_t *result, unsigned whence)
{
	int ret;
	ret = fs_llseek(fd, offset_high, offset_low, result, whence);
	if (TestControl.verbos)
		klog("llseek(%d, %x, %x, %x, %d) = %d, current %d\n", fd,
		     offset_high, offset_low, result, whence, ret,
		     (int)CURRENT_TASK()->fds[fd].fp->f_pos);

	return ret;
}

static int sys_select(int nfds, fd_set *readfds, fd_set *writefds,
		      fd_set *exceptfds, const struct timespec *timeout)
{
	if (TestControl.verbos)
		klog("select(%d, %x, %x, %x, %x)\n", nfds, readfds, writefds,
		     exceptfds, timeout);

	return do_select(nfds, readfds, writefds, exceptfds, timeout, NULL);
}

static int sys_newselect(int nfds, fd_set *readfds, fd_set *writefds,
			 fd_set *exceptfds, const struct timespec *timeout,
			 void *sigmask)
{
	if (TestControl.verbos)
		klog("pselect(%d, %x, %x, %x, %x, %x)\n", nfds, readfds,
		     writefds, exceptfds, timeout, sigmask);

	return do_select(nfds, readfds, writefds, exceptfds, timeout, sigmask);
}

static int sys_link(const char *path1, const char *path2)
{
	char *name1 = name_get();
	char *name2 = name_get();
	int ret = -1;
	resolve_path(path1, name1);
	resolve_path(path2, name2);

	ret = ext4_flink(name1, name2);

	if (TestControl.verbos)
		klog("symlink(%s, %s) = %d\n", name1, name2, ret);

	name_put(name1);
	name_put(name2);

	if (ret > 0)
		return (0 - ret);

	return ret;
}

static int sys_symlink(const char *path1, const char *path2)
{
	char *name1 = name_get();
	char *name2 = name_get();
	int ret = -1;
	resolve_path(path1, name1);
	resolve_path(path2, name2);

	ret = ext4_fsymlink(name1, name2);

	if (TestControl.verbos)
		klog("symlink(%s, %s) = %d\n", name1, name2, ret);

	name_put(name1);
	name_put(name2);

	if (ret > 0)
		return (0 - ret);

	return ret;
}

static int sys_chmod(const char *pathname, uint32_t mode)
{
	char *name = name_get();
	int ret = -1;
	resolve_path(pathname, name);
	ret = fs_chmod(name, mode);
	name_put(name);
	if (TestControl.verbos)
		klog("chmod(%s, %d) = %d\n", name, mode, ret);

	return ret;
}

static int sys_fchmod(int fd, uint32_t mode)
{
	int ret = -1;
	ret = fs_fchmod(fd, mode);
	if (TestControl.verbos)
		klog("chmod(%d, %d) = %d\n", fd, mode, ret);

	return ret;
}

static int sys_rename(const char *oldpath, const char *newpath)
{
	char *name1 = name_get();
	char *name2 = name_get();
	int ret = -1;

	if (!oldpath || !*oldpath || !newpath || !*newpath)
		return -ENOENT;

	resolve_path(oldpath, name1);
	resolve_path(newpath, name2);
	ret = ext4_frename(name1, name2);
	name_put(name1);
	name_put(name2);

	if (TestControl.verbos)
		klog("rename(%s, %s) = %d\n", name1, name2, ret);

	return (0 - ret);
}

static int sys_umask(unsigned mask)
{
	task_struct *cur = CURRENT_TASK();
	int ret = __sync_lock_test_and_set(&cur->umask, (mask & S_IRWXOGU));
	if (TestControl.verbos)
		klog("umask(%d) = %d\n", mask, ret);

	return ret;
}

static int sys_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	unsigned long long now = 0;
	if (!tv)
		return -EFAULT;

	now = time_now_ms();
	ms_to_timeval(now, tv);

	if (tz) {
		tz->tz_minuteswest = tz->tz_dsttime = 0;
	}

	if (TestControl.verbos)
		klog("gettimeofday() = %d(sec), %d(usec), while now is %d(ms)\n",
		     tv->tv_sec, tv->tv_usec, now);

	return 0;
}

static int sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
	// FIXME: no signal at all so rem never used
	unsigned int total_millisecond = 0;
	if (!req)
		return -EFAULT;

	if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec > 999999999)
		return -EINVAL;

	total_millisecond = req->tv_sec * 1000 + req->tv_nsec / 1000000;
	msleep(total_millisecond);
	return 0;
}

static int sys_setreuid(unsigned ruid, unsigned euid)
{
	if (TestControl.verbos)
		klog("setreuid(%d, %d)\n", ruid, euid);

	return 0;
}

static int sys_setregid(unsigned pid, unsigned pgid)
{
	if (TestControl.verbos)
		klog("setregid(%d, %d)\n", pid, pgid);

	return 0;
}

static void syscall_init()
{
	int_register(SYSCALL_INT_NO, syscall_process, 1, 3);
}

KERNEL_INIT(7, syscall_init);