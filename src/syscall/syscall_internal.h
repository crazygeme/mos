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
	       const struct timespec *timeout);
int sys_newselect(int nfds, fd_set *readfds, fd_set *writefds,
		  fd_set *exceptfds, const struct timespec *timeout,
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
int sys_readlink(const char *path, char *buf, unsigned bufsiz);
int sys_sync();
int sys_chdir(const char *path);
int sys_fchdir(int fd);
int sys_statfs(const char *path, struct statfs *buf);
int sys_fstatfs(int fd, struct statfs *buf);

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
int sys_kill(unsigned pid, int sig);
int sys_brk(unsigned top);
int sys_sched_yield();
unsigned sys_alarm(unsigned seconds);
int sys_pause();
int sys_sigaction(int sig, void *act, void *oact);
int sys_sigprocmask(int how, void *set, void *oset);
int sys_getrlimit(int resource, void *limit);
int sys_setrlimit(int resource, void *limit);
int sys_sigaltstack(const stack_t *ss, stack_t *old_ss);
long sys_personality(unsigned int personality);
int sys_getgroups(int size, unsigned *list);
int sys_setgroups(int size, unsigned short *list);
int sys_getgroups32(int size, unsigned *list);
int sys_setgroups32(int size, unsigned *list);
int sys_ugetrlimit(int resource, void *limit);
int sys_exit_group(int status);
int sys_query_module(const char *name, int which, void *buf, size_t bufsize,
		     size_t *ret);

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
int sys_mmap(struct mmap_arg_struct32 *arg);
int sys_mmap2(unsigned addr, unsigned len, unsigned prot, unsigned flags,
	      int fd, unsigned pgoffset);
int sys_munmap(void *addr, unsigned length);
int sys_mprotect(void *addr, unsigned len, int prot);
int sys_umask(unsigned mask);
int sys_sysinfo(void *info);

/*
 * syscall_net.c
 */
int sys_socketcall(int call, unsigned long *args);

#endif /* _SYSCALL_INTERNAL_H */
