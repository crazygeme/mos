#ifndef _SYSCALL_INTERNAL_H
#define _SYSCALL_INTERNAL_H
#include <fs/vfs.h>
#include <fs/select.h>
#include <fs/poll.h>
#include <ps/signal.h>
#include <stdint.h>

/*
 * Types used only within the syscall layer.
 */

struct utimbuf {
	unsigned actime;
	unsigned modtime;
};

struct itimerval {
	struct timeval it_interval;
	struct timeval it_value;
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
	char *iov_base;
	unsigned iov_len;
};

/*
 * syscall_io.c
 */
int sys_read(int fd, char *buf, unsigned len);
int sys_write(int fd, const char *buf, unsigned len);
int sys_pread64(int fd, void *buf, unsigned count, int offset);
int sys_pwrite64(int fd, const void *buf, unsigned count, int offset);
int sys_ioctl(int d, int request, char *buf);
int sys_open(const char *name, int flags, umode_t mode);
int sys_close(unsigned fd);
int sys_lseek(int fd, int offset, int whence);
int sys_llseek(int fd, unsigned offset_high, unsigned offset_low,
	       uint64_t *result, unsigned int whence);
int sys_readv(int fildes, const struct iovec *iov, int iovcnt);
int sys_writev(int fildes, const struct iovec *iov, int iovcnt);
int sys_fsync(int fd);
int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
int sys_pipe(int pipefd[2]);
int sys_fcntl(int fd, int cmd, int arg);
int sys_fcntl64(int fd, int cmd, int arg);
int sys_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
	       const struct timeval *timeout);
int sys_newselect(int nfds, fd_set *readfds, fd_set *writefds,
		  fd_set *exceptfds, const struct timeval *timeout,
		  void *sigmask);
int sys_poll(struct pollfd *fds, unsigned nfds, int timeout);
int sys_readdir(unsigned fd, struct linux_dirent *dirp, unsigned count);
int sys_getdents(unsigned int fd, struct linux_dirent *dirp,
		 unsigned int count);
int sys_getdents64(unsigned int fd, struct linux_dirent64 *dirp,
		   unsigned int count);

/*
 * syscall_fs.c
 */
int sys_stat(const char *pathname, struct stat *buf);
int sys_lstat(const char *path, struct stat *s);
int sys_fstat(int fd, struct stat *s);
int sys_stat64(const char *pathname, struct stat64 *s);
int sys_lstat64(const char *path, struct stat64 *s);
int sys_fstat64(int fd, struct stat64 *s);
int sys_oldstat(const char *filename, struct oldstat *buf);
int sys_access(const char *path, int mode);
int sys_chmod(const char *pathname, uint32_t mode);
int sys_fchmod(int fd, uint32_t mode);
int sys_chown(const char *pathname, uint32_t uid, uint32_t gid);
int sys_lchown(const char *pathname, uint32_t uid, uint32_t gid);
int sys_fchown(int fd, uint32_t uid, uint32_t gid);
int sys_link(const char *path1, const char *path2);
int sys_symlink(const char *path1, const char *path2);
int sys_unlink(const char *pathname);
int sys_rename(const char *oldpath, const char *newpath);
int sys_mknod(const char *path, unsigned mode, unsigned dev);
int sys_mkdir(const char *path, unsigned mode);
int sys_rmdir(const char *path);
int sys_creat(const char *path, unsigned mode);
int sys_mount(char *dev, char *dir_name, char *type, unsigned flag, void *data);
int sys_umount(char *name, int flag);
int sys_umount2(char *name, int flags);
int sys_readlink(const char *path, char *buf, unsigned bufsiz);
int sys_sync();
int sys_chdir(const char *path);
int sys_fchdir(int fd);
int sys_chroot(const char *path);
int sys_statfs(const char *path, struct statfs *buf);
int sys_fstatfs(int fd, struct statfs *buf);
int sys_flock(int fd, int operation);
int sys_ftruncate(int fd, unsigned long length);
int sys_ftruncate64(int fd, uint64_t length);
int sys__sysctl(void *args);
int sys_mlock(const void *addr, size_t len);
int sys_munlock(const void *addr, size_t len);
int sys_mlockall(int flags);
int sys_munlockall(void);
int sys_setxattr(const char *path, const char *name, const void *value,
		 unsigned size, int flags);
int sys_lsetxattr(const char *path, const char *name, const void *value,
		  unsigned size, int flags);
int sys_fsetxattr(int fd, const char *name, const void *value, unsigned size,
		  int flags);
int sys_getxattr(const char *path, const char *name, void *value,
		 unsigned size);
int sys_lgetxattr(const char *path, const char *name, void *value,
		  unsigned size);
int sys_fgetxattr(int fd, const char *name, void *value, unsigned size);
int sys_listxattr(const char *path, char *list, unsigned size);
int sys_llistxattr(const char *path, char *list, unsigned size);
int sys_flistxattr(int fd, char *list, unsigned size);
int sys_removexattr(const char *path, const char *name);
int sys_lremovexattr(const char *path, const char *name);
int sys_fremovexattr(int fd, const char *name);

/*
 * syscall_proc.c
 */
int sys_getpid();
int sys_getppid();
int sys_getpgrp(unsigned pid);
int sys_getpgid(unsigned pid);
int sys_setpgid(unsigned pid, unsigned pgid);
int sys_getsid(unsigned pid);
int sys_setsid();
int sys_getuid();
int sys_getgid();
int sys_geteuid();
int sys_getegid();
int sys_setuid(unsigned uid);
int sys_setgid(unsigned gid);
int sys_setreuid(unsigned ruid, unsigned euid);
int sys_setregid(unsigned rgid, unsigned egid);
int sys_setresuid(unsigned ruid, unsigned euid, unsigned suid);
int sys_getresuid(unsigned *ruid, unsigned *euid, unsigned *suid);
int sys_setresgid(unsigned rgid, unsigned egid, unsigned sgid);
int sys_getresgid(unsigned *rgid, unsigned *egid, unsigned *sgid);
int sys_setfsuid(unsigned fsuid);
int sys_setfsgid(unsigned fsgid);
/* 32-bit variants */
int sys_getuid32(void);
int sys_getgid32(void);
int sys_geteuid32(void);
int sys_getegid32(void);
int sys_setuid32(unsigned uid);
int sys_setgid32(unsigned gid);
int sys_setreuid32(unsigned ruid, unsigned euid);
int sys_setregid32(unsigned rgid, unsigned egid);
int sys_setresuid32(unsigned r, unsigned e, unsigned s);
int sys_getresuid32(unsigned *r, unsigned *e, unsigned *s);
int sys_setresgid32(unsigned r, unsigned e, unsigned s);
int sys_getresgid32(unsigned *r, unsigned *e, unsigned *s);
int sys_setfsuid32(unsigned fsuid);
int sys_setfsgid32(unsigned fsgid);
int sys_wait4(int pid, int *status, int options, void *rusage);
int sys_brk(unsigned top);
int sys_sched_yield();
unsigned sys_alarm(unsigned seconds);
int sys_getrlimit(int resource, void *limit);
int sys_setrlimit(int resource, void *limit);
long sys_personality(unsigned int personality);
int sys_getgroups(int size, unsigned *list);
int sys_setgroups(int size, unsigned short *list);
int sys_getgroups32(int size, unsigned *list);
int sys_setgroups32(int size, unsigned *list);
int sys_ugetrlimit(int resource, void *limit);
int sys_exit_group(int status);
int sys_query_module(const char *name, int which, void *buf, size_t bufsize,
		     size_t *ret);
int sys_gettid(void);
int sys_tkill(int tid, int sig);
int sys_nice(int inc);
int sys_acct(const char *filename);
int sys_futex(int *uaddr, int op, int val, const struct timespec *timeout,
	      int *uaddr2, int val3);
int sys_set_thread_area(void *u_info);
int sys_get_thread_area(void *u_info);
int sys_set_tid_address(int *tidptr);
int sys_modify_ldt(int func, void *ptr, unsigned long bytecount);
int sys_setitimer(int which, const struct itimerval *new_value,
		  struct itimerval *old_value);
int sys_getitimer(int which, struct itimerval *value);
int sys_clone(unsigned long flags, unsigned long child_stack,
	      int *parent_tidptr, int *tls, int *child_tidptr);

/*
 * ps/ps_signal.c
 */
int sys_kill(int pid, int sig);
int sys_pause();
void *sys_signal(int sig, void *handler);
int sys_sigaction(int sig, void *act, void *oact);
int sys_rt_sigaction(int sig, void *act, void *oact, unsigned sigsetsize);
int sys_sigprocmask(int how, void *set, void *oset);
int sys_rt_sigprocmask(int how, void *set, void *oset, unsigned sigsetsize);
int sys_sigreturn(void);
int sys_rt_sigreturn(void);
int sys_sigaltstack(const stack_t *ss, stack_t *old_ss);
int sys_rt_sigpending(sigset_t *set, unsigned sigsetsize);
int sys_rt_sigtimedwait(const sigset_t *set, void *info,
			const struct timespec *timeout, unsigned sigsetsize);
int sys_rt_sigqueueinfo(unsigned pid, int sig, void *uinfo);
int sys_rt_sigsuspend(const sigset_t *mask, unsigned sigsetsize);

/*
 * syscall_sys.c
 */
struct tms {
	long tms_utime;
	long tms_stime;
	long tms_cutime;
	long tms_cstime;
};
long sys_times(struct tms *buf);
int sys_setpriority(int which, int who, int prio);
int sys_vhangup(void);
int sys_uname(struct utsname *utname);
int sys_sethostname(const char *name, unsigned len);
int sys_utime(const char *filename, const struct utimbuf *times);
int sys_time(unsigned *t);
int sys_gettimeofday(struct timeval *tv, struct timezone *tz);
int sys_settimeofday(const struct timeval *tv, const struct timezone *tz);
int sys_nanosleep(const struct timespec *req, struct timespec *rem);
int sys_reboot(unsigned magic1, unsigned magic2, unsigned cmd, void *arg);
int sys_getpriority(int which, int who);
int sys_ioperm(unsigned long from, unsigned long num, int turn_on);
int sys_iopl(int level);
int sys_vm86old(void *user_vm86);
int sys_vm86(unsigned long fn, void *user_vm86plus);
int sys_quotactl(int cmd, const char *special, int id, void *addr);
int sys_mmap(struct mmap_arg_struct32 *arg);
int sys_mmap2(unsigned addr, unsigned len, unsigned prot, unsigned flags,
	      int fd, unsigned pgoffset);
int sys_munmap(void *addr, unsigned length);
int sys_mprotect(void *addr, unsigned len, int prot);
int sys_mremap(unsigned old_addr, unsigned old_size, unsigned new_size,
	       int flags, unsigned new_addr);
int sys_sched_setparam(int pid, const void *param);
int sys_sched_getparam(int pid, void *param);
int sys_sched_setscheduler(int pid, int policy, const void *param);
int sys_sched_getscheduler(int pid);
int sys_sched_get_priority_max(int algorithm);
int sys_sched_get_priority_min(int algorithm);
int sys_sched_rr_get_interval(int pid, struct timespec *tp);
int sys_umask(unsigned mask);
int sys_sysinfo(void *info);
int sys_break(void);
int sys_stime(unsigned *t);
int sys_stty(void);
int sys_gtty(void);
int sys_ftime(void *tp);
int sys_prof(void);
int sys_lock(void);
int sys_ipc(unsigned call, int first, int second, int third, void *ptr,
	    long fifth);

int sys_readahead(int fd, unsigned offset_hi, unsigned offset_lo,
		  unsigned count);

/*
 * syscall_net.c
 */
int sys_socketcall(int call, unsigned long *args);

#endif /* _SYSCALL_INTERNAL_H */
