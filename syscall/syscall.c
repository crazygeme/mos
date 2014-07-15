#include <syscall/syscall.h>
#include <int/int.h>
#include <syscall/unistd.h>
#include <ps/ps.h>
#include <user/ps0.h>
#include <config.h>
#include <fs/namespace.h>
#include <int/timer.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <fs/fcntl.h>
#include <errno.h>


struct utimbuf {
	unsigned actime;       /* access time */
	unsigned modtime;      /* modification time */
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
	char   *iov_base;  /* Base address. */
	unsigned iov_len;    /* Length. */
};

struct linux_dirent
{
	unsigned long  d_ino;     /* Inode number */
	unsigned long  d_off;     /* Offset to next linux_dirent */
	unsigned short d_reclen;  /* Length of this linux_dirent */
	char           d_name[];  /* Filename (null-terminated) */
	/* length is actually (d_reclen - 2 -
	   offsetof(struct linux_dirent, d_name) */
	/*
	char           pad;       // Zero padding byte
	char           d_type;    // File type (only since Linux 2.6.4;
							  // offset is (d_reclen - 1))
	*/

};

    
  enum {
          UNAME26 =               0x0020000,
          ADDR_NO_RANDOMIZE =     0x0040000,      /* disable randomization of VA space */
          FDPIC_FUNCPTRS =        0x0080000,      /* userspace function ptrs point to descriptors
                                                   * (signal handling)
                                                   */
          MMAP_PAGE_ZERO =        0x0100000,
          ADDR_COMPAT_LAYOUT =    0x0200000,
          READ_IMPLIES_EXEC =     0x0400000,
          ADDR_LIMIT_32BIT =      0x0800000,
          SHORT_INODE =           0x1000000,
          WHOLE_SECONDS =         0x2000000,
          STICKY_TIMEOUTS =       0x4000000,
          ADDR_LIMIT_3GB =        0x8000000,
  };
  
  /*
   * Security-relevant compatibility flags that must be
   * cleared upon setuid or setgid exec:
   */
  #define PER_CLEAR_ON_SETID (READ_IMPLIES_EXEC  | \
                              ADDR_NO_RANDOMIZE  | \
                              ADDR_COMPAT_LAYOUT | \
                              MMAP_PAGE_ZERO)

  enum {
          PER_LINUX =             0x0000,
          PER_LINUX_32BIT =       0x0000 | ADDR_LIMIT_32BIT,
          PER_LINUX_FDPIC =       0x0000 | FDPIC_FUNCPTRS,
          PER_SVR4 =              0x0001 | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
          PER_SVR3 =              0x0002 | STICKY_TIMEOUTS | SHORT_INODE,
          PER_SCOSVR3 =           0x0003 | STICKY_TIMEOUTS |
                                           WHOLE_SECONDS | SHORT_INODE,
          PER_OSR5 =              0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS,
          PER_WYSEV386 =          0x0004 | STICKY_TIMEOUTS | SHORT_INODE,
          PER_ISCR4 =             0x0005 | STICKY_TIMEOUTS,
          PER_BSD =               0x0006,
          PER_SUNOS =             0x0006 | STICKY_TIMEOUTS,
          PER_XENIX =             0x0007 | STICKY_TIMEOUTS | SHORT_INODE,
          PER_LINUX32 =           0x0008,
          PER_LINUX32_3GB =       0x0008 | ADDR_LIMIT_3GB,
          PER_IRIX32 =            0x0009 | STICKY_TIMEOUTS,/* IRIX5 32-bit */
          PER_IRIXN32 =           0x000a | STICKY_TIMEOUTS,/* IRIX6 new 32-bit */
          PER_IRIX64 =            0x000b | STICKY_TIMEOUTS,/* IRIX6 64-bit */
          PER_RISCOS =            0x000c,
          PER_SOLARIS =           0x000d | STICKY_TIMEOUTS,
          PER_UW7 =               0x000e | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
          PER_OSF4 =              0x000f,                  /* OSF/1 v4 */
          PER_HPUX =              0x0010,
          PER_MASK =              0x00ff,
  };


static int test_call(unsigned arg0, unsigned arg1, unsigned arg2);
static int sys_read(unsigned fd, char* buf, unsigned len);
static int sys_write(unsigned fd, char* buf, unsigned len);
static int sys_getpid();
static int sys_uname(struct utsname* utname);
static int sys_open(const char* name, int flags, unsigned mode);
static int sys_close(unsigned fd);
static int sys_sched_yield();
static int sys_brk(unsigned top);
static int sys_chdir(const char *path);
static int sys_ioctl(int d, int request, char* buf);
static int sys_creat(const char* path, unsigned mode);
static int sys_mkdir(const char* path, unsigned mode);
static int sys_rmdir(const char* path);
static int sys_reboot(unsigned cmd);
static int sys_getuid();
static int sys_getgid();
static int sys_geteuid();
static int sys_getegid();
static int sys_mmap(struct mmap_arg_struct32* arg);
static int sys_mprotect(void *addr, unsigned len, int prot);
static int sys_writev(int fildes, const struct iovec *iov, int iovcnt);
static long sys_personality(unsigned int personality);
static int sys_fcntl(int fd, int cmd, int arg);
static int sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count);
static int sys_lstat64(const char* path, struct stat64* s);
static int sys_fstat(int fd, struct stat64* s);
static int sys_fstat64(int fd, struct stat64* s);
static int sys_munmap(void *addr, unsigned length);
static int sys_getppid();
static int sys_getpgrp(unsigned pid);
static int sys_setpgid(unsigned pid, unsigned pgid);
static int sys_wait4(int pid, int* status, void* rusage);
static int sys_socketcall(int call, unsigned long *args);
static int sys_sigaction(int sig, void* act, void*  oact);
static int sys_sigprocmask(int how, void* set, void * oset);
static int sys_pause();
static int sys_utime(const char *filename, const struct utimbuf *times);
static int sys_quota(struct krnquota* quota);
static int sys_pipe(int pipefd[2]);
static int sys_dup(int oldfd);
static int sys_dup2(int oldfd, int newfd);
static int sys_getrlimit(int resource, void* limit);
static int sys_kill(unsigned pid, int sig);
static int sys_unlink(const char *pathname);

static int resolve_path(char* old, char* new);

static char* call_table_name[NR_syscalls] = {
	"test_call", 
    "sys_exit", "sys_fork", "sys_read", "sys_write", "sys_open",   // 1  ~ 5
    "sys_close", "sys_waitpid", "sys_creat", 0, "sys_unlink",           // 6  ~ 10  
    "sys_execve", "sys_chdir", "time", 0, 0,           // 11 ~ 15
    0, 0, 0, 0, "sys_getpid",  // 16 ~ 20   
    0, 0, 0, "sys_getuid", 0,          // 21 ~ 25 
    0, 0, 0, "sys_pause", "sys_utime",          // 26 ~ 30 
    0, 0, 0, 0, 0,          // 31 ~ 35 
    0, "sys_kill", 0, "sys_mkdir", "sys_rmdir",          // 36 ~ 40 
    "sys_dup", "sys_pipe", 0, 0, "sys_brk",          // 41 ~ 45 
    0, "sys_getgid", 0, "sys_geteuid", "sys_getegid",          // 46 ~ 50 
    0, 0, 0, "sys_ioctl", "sys_fcntl",          // 51 ~ 55 
    0, "sys_setpgid", 0, 0, 0,          // 56 ~ 60 
    0, 0, "sys_dup2", "sys_getppid", "sys_getpgrp",          // 61 ~ 65 
    0, 0, 0, 0, 0,          // 66 ~ 70 
    0, 0, 0, 0, 0,          // 71 ~ 75 
    "sys_getrlimit", 0, 0, 0, 0,          // 76 ~ 80 
    0, 0, 0, 0, 0,          // 81 ~ 85 
    0, 0, "sys_reboot", "sys_readdir", "sys_mmap",          // 86 ~ 90 
    "sys_munmap", 0, 0, 0, 0,          // 91 ~ 95 
    0, 0, 0, 0, 0,          // 96 ~ 100 
    0, "sys_socketcall", 0, 0, 0,          // 101 ~ 105
    "stat", 0, "fs_fstat", 0, 0,          // 106 ~ 110 
    0, 0, 0, "sys_wait4", 0,          // 111 ~ 115
    0, 0, 0, 0, 0,          // 116 ~ 120
    0, "sys_uname", 0, 0, "sys_mprotect",          // 121 ~ 125
    0, 0, 0, 0, 0,          // 126 ~ 130
    0, 0, 0, 0, 0,          // 131 ~ 135
    "sys_personality", 0, 0, 0, 0,          // 136 ~ 140
    "sys_getdents", 0, 0, 0, 0,          // 141 ~ 145
    "sys_writev", 0, 0, 0, 0,          // 146 ~ 150
    0, 0, 0, 0, 0,          // 151 ~ 155
    0, 0, "sys_sched_yield", 0, 0,          // 156 ~ 160
    0, 0, 0, 0, 0,          // 161 ~ 165
    0, 0, 0, 0, 0,          // 165 ~ 170
    0, 0, 0, "sys_sigaction", "sys_sigprocmask",          // 171 ~ 175
    0, 0, 0, 0, 0,          // 175 ~ 180
    0, 0, "sys_getcwd", 0, 0,          // 181 ~ 185
    0, 0, 0, 0, 0,          // 185 ~ 190
    0, 0, 0, 0, 0,            // 191 ~ 195
	"sys_lstat64", "sys_fstat64" , "sys_quota"             // 196 ~ 198
};

extern hash_table* file_exist_cache;

typedef int (*syscall_fn)(unsigned ebx, unsigned ecx, unsigned edx);

static unsigned call_table[NR_syscalls] = {
	test_call, 
    sys_exit, sys_fork, sys_read, sys_write, sys_open,   // 1  ~ 5
    sys_close, sys_waitpid, sys_creat, 0, sys_unlink,           // 6  ~ 10  
    sys_execve, sys_chdir, time, 0, 0,           // 11 ~ 15
    0, 0, 0, 0, sys_getpid,  // 16 ~ 20   
    0, 0, 0, sys_getuid, 0,          // 21 ~ 25 
    0, 0, 0, sys_pause, sys_utime,          // 26 ~ 30 
    0, 0, 0, 0, 0,          // 31 ~ 35 
    0, sys_kill, 0, sys_mkdir, sys_rmdir,          // 36 ~ 40 
    sys_dup, sys_pipe, 0, 0, sys_brk,          // 41 ~ 45 
    0, sys_getgid, 0, sys_geteuid, sys_getegid,          // 46 ~ 50 
    0, 0, 0, sys_ioctl, sys_fcntl,          // 51 ~ 55 
    0, sys_setpgid, 0, 0, 0,          // 56 ~ 60 
    0, 0, sys_dup2, sys_getppid, sys_getpgrp,          // 61 ~ 65 
    0, 0, 0, 0, 0,          // 66 ~ 70 
    0, 0, 0, 0, 0,          // 71 ~ 75 
    sys_getrlimit, 0, 0, 0, 0,          // 76 ~ 80 
    0, 0, 0, 0, 0,          // 81 ~ 85 
    0, 0, sys_reboot, sys_readdir, sys_mmap,          // 86 ~ 90 
    sys_munmap, 0, 0, 0, 0,          // 91 ~ 95 
    0, 0, 0, 0, 0,          // 96 ~ 100 
    0, sys_socketcall, 0, 0, 0,          // 101 ~ 105
    fs_stat, 0, fs_fstat, 0, 0,          // 106 ~ 110 
    0, 0, 0, sys_wait4, 0,          // 111 ~ 115
    0, 0, 0, 0, 0,          // 116 ~ 120
    0, sys_uname, 0, 0, sys_mprotect,          // 121 ~ 125
    0, 0, 0, 0, 0,          // 126 ~ 130
    0, 0, 0, 0, 0,          // 131 ~ 135
    sys_personality, 0, 0, 0, 0,          // 136 ~ 140
    sys_getdents, 0, 0, 0, 0,          // 141 ~ 145
    sys_writev, 0, 0, 0, 0,          // 146 ~ 150
    0, 0, 0, 0, 0,          // 151 ~ 155
    0, 0, sys_sched_yield, 0, 0,          // 156 ~ 160
    0, 0, 0, 0, 0,          // 161 ~ 165
    0, 0, 0, 0, 0,          // 165 ~ 170
    0, 0, 0, sys_sigaction, sys_sigprocmask,          // 171 ~ 175
    0, 0, 0, 0, 0,          // 175 ~ 180
    0, 0, sys_getcwd, 0, 0,          // 181 ~ 185
    0, 0, 0, 0, 0,          // 185 ~ 190
    0, 0, 0, 0, 0,            // 191 ~ 195
	sys_lstat64, sys_fstat64, sys_quota            // 196 ~ 198
};

static int unhandled_syscall(unsigned callno)
{
	#ifdef __VERBOS_SYSCALL__
	klog("unhandled syscall %d\n", callno);
	#endif
    return -1;
}



static void syscall_process(intr_frame* frame)
{
	syscall_fn fn = call_table[frame->eax];
	int ret = 0;
	if ( !fn )
	{
		return unhandled_syscall(frame->eax);
	}

	ret = (unsigned)fn(frame->ebx, frame->ecx, frame->edx);


	__asm__("movl %0, %%eax" : : "m"(ret));
	return;
}

void syscall_init()
{
	int_register(SYSCALL_INT_NO, syscall_process, 1, 3);
}

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2)
{
	printk("test call: arg0 %x, arg1 %x, arg2 %x\n", arg0, arg1, arg2);
	return 0;
}

static int sys_read(unsigned fd, char* buf, unsigned len)
{
	task_struct* cur = CURRENT_TASK();
	unsigned ino = 0;
	int i = 0, pri_len = 0;

	if (fd > MAX_FD)
		return -1;

	if (cur->fds[fd].flag == 0)
		return -1;

	if (cur->fds[fd].flag & fd_flag_isdir)
		return -1;

	ino = cur->fds[fd].file;
	
		unsigned offset = cur->fds[fd].file_off;
		len = fs_read(fd, offset, buf, len);
		offset += len;
		cur->fds[fd].file_off = offset;
	

	#ifdef __VERBOS_SYSCALL__
	pri_len = (len > 5) ? 5 : len;
	klog("read(%d, \"", fd);
	for (i = 0; i< pri_len; i++){
		if (isprint(buf[i]))
			klog_printf("%c", buf[i]);
		else
			klog_printf("\\%x", buf[i]);
	}
	klog_printf("\", %d)\n", len);
	#endif



	return len;
}

static int sys_write(unsigned fd, char *buf, unsigned len)
{
	task_struct *cur = CURRENT_TASK();
	unsigned ino = 0;
	unsigned _len;
#ifdef __VERBOS_SYSCALL__
	char *tmp;
	tmp = kmalloc(len + 1);
	memset(tmp, 0, len + 1);
	memcpy(tmp, buf, len);
	klog("write(%d, \"%s\", %d) ", fd, tmp, len);
	kfree(tmp);
#endif

	if (fd > MAX_FD)
	{
		//printf("-1\n");
		return -1;
	}

	if (cur->fds[fd].flag == 0)
	{
#ifdef __VERBOS_SYSCALL__
		klog_printf("ret -1\n");
#endif
		return -1;
	}

	if (cur->fds[fd].flag & fd_flag_isdir)
	{
#ifdef __VERBOS_SYSCALL__
		klog_printf("ret -1\n");
#endif
		return -1;
	}


	ino = cur->fds[fd].file;

	unsigned offset = cur->fds[fd].file_off;
	_len = fs_write(fd, offset, buf, len);
	offset += _len;
	cur->fds[fd].file_off = offset;

#ifdef __VERBOS_SYSCALL__
	klog_printf("ret %d\n", _len);
#endif
	return _len;
}

static int sys_ioctl(int fd, int request, char *buf)
{
	task_struct *cur = CURRENT_TASK();
	unsigned ino = 0;

	//printf("ioctl(%d, %d, %x)\n", fd, request, buf);
	if (fd > MAX_FD) return 0;

	if (cur->fds[fd].flag == 0) return 0;

	if (cur->fds[fd].flag & fd_flag_isdir) return 0;


	if (request == IOCTL_TTY)
	{
		return tty_ioctl(buf);
	} else if (request == 0x5401)
	{
		struct termios *s = (struct termios *)buf;
		char tmp[] = { 0x03, 0x1c, 0x7f, 0x15, 0x04,
			0x00, 0x01, 0x00, 0x11, 0x13,
			0x1a, 0x00, 0x12, 0x0f, 0x17,
			0x16, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00 };
		s->c_iflag  = 0x500;
		s->c_oflag  = 0x5;
		s->c_cflag  = 0xbf;
		s->c_lflag  = 0x8a3b;
		s->c_line = 0x0;
		memcpy(s->c_cc, tmp, 16);
//  	s->c_ispeed  = 0xf;
//  	s->c_ospeed  = 0xf;
		return 0;
	} else
	{
		return 0;
	}

	return 0;
}

static int sys_getpid()
{
	task_struct *cur = CURRENT_TASK();
	return cur->psid;
}

static int sys_uname(struct utsname *utname)
{
	strcpy(utname->machine, "i386");
	strcpy(utname->nodename, "qemu-enum");
	strcpy(utname->release, "0.1-generic");
	strcpy(utname->sysname, "Mos");
	strcpy(utname->version, "Mos Wed Feb 19 04:14:56 UTC 2014");
	strcpy(utname->domain, "Ender");
	return 1;
}

static int sys_sched_yield()
{
	task_sched();
	return 0;
}

static int sys_open(const char *_name, int flags, unsigned mode)
{
	char *name = kmalloc(64);
	int creat_if_not_exist = 0;
	int resize_to_zero = 0;
	int open_mode = flags & O_ACCMODE;
	int close_on_exec = flags & FD_CLOEXEC;
	struct stat s;
	int fd = -1;
    key_value_pair* pair = 0;
	task_struct *cur = CURRENT_TASK();

	if (flags & O_CREAT) creat_if_not_exist = 1;

	if (flags & O_TRUNC) resize_to_zero = 1;

	resolve_path(_name, name);


#ifdef __VERBOS_SYSCALL__
	klog("open(%s, %x, %x)", name, flags, mode);
#endif


	if (fs_stat(name, &s) == -1)
	{
		if (creat_if_not_exist)
		{
			fs_create(name, mode);
        }
	} else
	{
		if (resize_to_zero)
		{
			fs_delete(name);
			fs_create(name, s.st_mode);
		}
	}


	fd = fs_open(name);

	if (fd >= 0 && close_on_exec)
	{
		cur->fds[fd].flag |= fd_flag_closeexec;
	}

#ifdef __VERBOS_SYSCALL__
	klog_printf("ret %d\n", fd);
#endif

	kfree(name);
    return fd; 
}

static int sys_close(unsigned fd)
{
	#ifdef __VERBOS_SYSCALL__
	klog("close(%d)\n", fd);
	#endif
	fs_close(fd);
	return 0;
}

static int sys_brk(unsigned top)
{
	task_struct *task = CURRENT_TASK();
	unsigned size = 0;
	unsigned pages = 0;

	if (task->user.heap_top == USER_HEAP_BEGIN)
	{
		mm_add_dynamic_map(task->user.heap_top, 0, PAGE_ENTRY_USER_DATA);
		task->user.heap_top += PAGE_SIZE;
	}


	if (top == 0)
	{
		return task->user.heap_top;
	} else if (top >= USER_HEAP_END)
	{
		return task->user.heap_top;
	} else if (top > task->user.heap_top)
	{
		int i = 0;
		size = top - task->user.heap_top;
		pages = (size - 1) / PAGE_SIZE + 1;

		for (i = 0; i < pages; i++)
		{
			unsigned vir = task->user.heap_top + i * PAGE_SIZE;
			mm_add_dynamic_map(vir, 0, PAGE_ENTRY_USER_DATA);
		}

		top = task->user.heap_top + pages * PAGE_SIZE;
		task->user.heap_top = top;
		return top;

	} else
	{
		int i = 0;

		if (top < USER_HEAP_BEGIN) top = USER_HEAP_BEGIN;

		size = task->user.heap_top - top;
		pages = (size - 1) / PAGE_SIZE + 1;
		for (i = 0; i < pages; i++)
		{
			unsigned vir = top + i * PAGE_SIZE;
			mm_del_dynamic_map(vir);
		}
#ifdef __VERBOS_SYSCALL__
		klog("brk: cur %x newtop %x, ret %x\n", task->user.heap_top, top, top);
#endif
		task->user.heap_top = top;

		return top;

	}

	return 0;
}

static int _chdir(const char *cwd, const char *path)
{
	struct stat s;

	if (!path) return 0;

	if (!*path) return 1;

	if (!strcmp(path, ".")) return 1;

	if (!strcmp(path, ".."))
	{
		char *tmp;
		tmp = strrchr(cwd, '/');
		if (tmp) *tmp = '\0';
		strcpy(cwd, cwd);
		return 1;
	}

	strcat(cwd, "/");
	strcat(cwd, path);
	if (fs_stat(cwd, &s) == -1) return 0;

	if (!S_ISDIR(s.st_mode)) return 0;

	strcpy(cwd, cwd);
	return 1;
}

static int sys_chdir(const char *path)
{
	char name[64] = { 0 };
	char *slash, *tmp;
	char cwd[64] = { 0 };
	int ret = 1;
	task_struct *cur = CURRENT_TASK();



	if (!path || !*path) return 0;

	if (*path != '/') strcpy(cwd, cur->cwd);

	strcpy(name, path);
	slash = strchr(name, '/');
	tmp = name;
	while (slash)
	{
		*slash = '\0';
		ret = _chdir(cwd, tmp);
		if (!ret) return 0;

		slash++;
		tmp = slash;
		slash = strchr(tmp, '/');
	}

	ret = _chdir(cwd, tmp);
	if (!ret) return -1;

	strcpy(cur->cwd, cwd);
	return 0;
}





static int sys_creat(const char *path, unsigned mode)
{
	struct stat s;
	if (fs_stat(path, &s) != -1) return 0;
	return fs_create(path, mode);
}

static int sys_mkdir(const char *path, unsigned mode)
{
	mode |= S_IFDIR;
	return sys_creat(path, mode);
}

static int sys_rmdir(const char *path)
{
	struct stat s;
	if (fs_stat(path, &s) == -1) return 0;

	if (!S_ISDIR(s.st_mode)) return 0;

	if (s.st_size) return 0;

	return fs_delete(path);
}

static int sys_reboot(unsigned cmd)
{
	switch (cmd)
	{
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
	return 0;
}

static int sys_getgid()
{
	return 0;
}

static int sys_geteuid()
{
	return 0;
}

static int sys_getegid()
{
	return 0;
}
static int debug = 0;

static int sys_mmap(struct mmap_arg_struct32 *arg)
{
	int vir = 0;

	if (arg->len > (10 * 1024 * 1024))
	{
		return -1;
	}

	vir = do_mmap(arg->addr, arg->len, arg->prot, arg->flags, arg->fd, arg->offset);

#ifdef __VERBOS_SYSCALL__
	klog("mmap: fd %d, addr %x, offset %x, len %x at addr %x\n",
		 arg->fd, arg->addr, arg->offset, arg->len, vir);
#endif

	return vir;
}

static int sys_munmap(void *addr, unsigned length)
{
	// FIXME
	return 0;
}

static int sys_mprotect(void *addr, unsigned len, int prot)
{
#ifdef __VERBOS_SYSCALL__
	klog("mprotect: addr %x, len %x, prot %x\n", addr, len, prot);
#endif
	return 0;
}

static int sys_writev(int fildes, const struct iovec *iov, int iovcnt)
{
	int i = 0;
	unsigned total = 0;
	for (i = 0; i < iovcnt; i++)
	{
		total += sys_write(fildes, iov[i].iov_base, iov[i].iov_len);

	}
	return total;
}

static long sys_personality(unsigned int personality)
{
	return PER_LINUX32_3GB;
}

static int sys_fcntl(int fd, int cmd, int arg)
{
	return 0;
}


#define ROUND_UP(x) (((x)+sizeof(long)-1) & ~(sizeof(long)-1))
#define NAME_OFFSET(de) ((int) ((de)->d_name - (char *) (de)))
static int sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count)
{

	/*
	static unsigned char buf[] = {
		0xa0, 0x1c, 0x76, 0x00, 0x79, 0xd3, 0xe9, 0x0b, 0x18, 0x00, 0x64, 0x69, 0x72, 0x5f, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x63, 0x00, 0x00, 0x00, 0x08, //  . . v . y . . . . . d i r _ t e s t . c . . . .
0x01, 0x00, 0x76, 0x00, 0xe4, 0xd5, 0x29, 0x32, 0x10, 0x00, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x04, //  . . v . . . ) 2 . . . . . . . .
0x12, 0x08, 0x80, 0x00, 0x1a, 0xa0, 0xa7, 0x4b, 0x10, 0x00, 0x6c, 0x69, 0x62, 0x00, 0x00, 0x04, //  . . . . . . . K . . l i b . . .
0x48, 0x04, 0xa0, 0x03, 0x97, 0x7e, 0xbc, 0x52, 0x10, 0x00, 0x2e, 0x2e, 0x00, 0x00, 0x00, 0x04, //  H . . . . ~ . R . . . . . . . .
0x03, 0x00, 0x76, 0x00, 0xaa, 0xa0, 0x07, 0x58, 0x10, 0x00, 0x62, 0x69, 0x6e, 0x00, 0x00, 0x04, //  . . v . . . . X . . b i n . . .
0x05, 0x00, 0x76, 0x00, 0x37, 0x82, 0x10, 0x76, 0x10, 0x00, 0x65, 0x74, 0x63, 0x00, 0x00, 0x04, //  . . v . 7 . . v . . e t c . . .
0x02, 0x00, 0x76, 0x00, 0xff, 0xff, 0xff, 0x7f, 0x14, 0x00, 0x64, 0x69, 0x72, 0x5f, 0x74, 0x65, 0x73, 0x74, 0x00, 0x08, //  . . v . . . . . . . d i r _ t e s t . .
	};
	memcpy(dirp, buf, sizeof(buf));
	return (sizeof(buf)); 
	*/

	struct dirent d;
	int retcount;
	int len;
	int ret = 0;
	int cur_pos = 0;
	struct linux_dirent *prev;

#ifdef __VERBOS_SYSCALL__
	klog("getdents(%d, %x, %d)", fd, dirp, count);
#endif

	if (fd < 0 || fd >= MAX_FD)
	{
#ifdef __VERBOS_SYSCALL__
		klog_printf(" = %d\n", -9);
#endif
		return -9;
	}



	if (count < sizeof(struct linux_dirent))
	{
#ifdef __VERBOS_SYSCALL__
		klog_printf(" = %d\n", -22);
#endif
		return -22;
	}

	retcount = 0;
	prev = 0;


	while (count > 0)
	{


		ret = sys_readdir(fd, &d);
		if (ret == 0)
		{
			if (prev)
			{
				prev->d_off = 512;
			}
			memset((char *)dirp, 0, count);
			break;
		}

		len = ROUND_UP(NAME_OFFSET(dirp) + strlen(d.d_name) + 1);
		memset(dirp, 0, len);
		dirp->d_ino = d.d_ino;
		strcpy(dirp->d_name, d.d_name);

		dirp->d_reclen = ROUND_UP(NAME_OFFSET(dirp) + strlen(dirp->d_name) + 1);

		cur_pos += dirp->d_reclen;
		dirp->d_off = cur_pos;


		if (count <= dirp->d_reclen)
		{
			break;
		}
		retcount += dirp->d_reclen;
		count -= dirp->d_reclen;
		prev = dirp;
		dirp = (char *)dirp + dirp->d_reclen;

	}
#ifdef __VERBOS_SYSCALL__
	klog_printf(" = %d\n", retcount);
#endif
	return retcount;

}

static int sys_fstat(int fd, struct stat64 *s)
{
	return sys_fstat64(fd, s);
}

static int sys_fstat64(int fd, struct stat64 *s)
{
	// FIXME
	// rewrite it all please
	task_struct *cur = CURRENT_TASK();
	struct stat s32;

	if (fd > MAX_FD) return -1;

#ifdef __VERBOS_SYSCALL__
	klog("fstat64(%d, %x) size %d blocks %d blksize %d\n",
		 fd, s, s32.st_size, s32.st_blocks, s32.st_blksize);
#endif

	fs_fstat(fd, &s32);
	s->st_dev = s32.st_dev;
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

	return 0;

}

static int sys_lstat64(const char *path, struct stat64 *s)
{
	struct stat s32;

	char *new = kmalloc(64);
	resolve_path(path, new);
	fs_stat(new, &s32);

#ifdef __VERBOS_SYSCALL__
	klog("lstat64(%s, %x) blksize %d blocks %d, size %d \n", new, s,
		 s32.st_blksize, s32.st_blocks, s32.st_size);
#endif

	memset(s, 0, sizeof(*s));
	s->st_dev = s32.st_dev;
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
	kfree(new);

	return 0;
}

static int sys_getppid()
{
	task_struct *cur = CURRENT_TASK();
	return cur->parent;
}

static int sys_getpgrp(unsigned pid)
{
	// FIXME
	return 0;
}

static int sys_setpgid(unsigned pid, unsigned pgid)
{
	// FIXME
	if (pid == 0)
	{
		task_struct *cur = CURRENT_TASK();
		cur->group_id = pgid;

	}
	return 0;
}

static int sys_wait4(int pid, int *status, void *rusage)
{
	if (pid == -1)
	{
		return sys_waitpid(0, status, 0);
	} else if (pid < -1)
	{
		// FIXME
		// wait on group id
		return 0;
	} else
	{
		return sys_waitpid(pid, status, 0);
	}
}

static int sys_socketcall(int call, unsigned long *args)
{
	// FIXME
	// no socket at all right now
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
	__asm__("hlt");
	return -1;
}

static int resolve_path(char *old, char *new)
{
	if (!old || !*old) return -1;

	if (!strcmp(old, "."))
	{
		sys_getcwd(new, 64);
		return 0;
	}

	if (old[0] == '/')
	{
		strcpy(new, old);
		return 0;
	} else if (strlen(old) > 1 && old[0] == '.' && old[1] == '/') old += 2;

	sys_getcwd(new, 64);
	if (strcmp(new, "/")) strcat(new, "/");
	strcat(new, old);
	return 0;
}

static int sys_utime(const char *filename, const struct utimbuf *times)
{
	// FIXME
	return 0;
}

extern unsigned int heap_quota;
extern unsigned int heap_quota_high;
extern unsigned int cur_block_top;
extern unsigned phymm_cur;
extern unsigned phymm_high;
extern unsigned phymm_max;
extern unsigned pgc_count;
extern unsigned pgc_top;
extern unsigned task_schedule_time;
static int sys_quota(struct krnquota *quota)
{
	quota->heap_cur = heap_quota;
	quota->heap_wm = heap_quota_high;
	quota->heap_top = cur_block_top;
	quota->phymm_cur = phymm_cur;
	quota->phymm_wm = phymm_high;
	quota->phymm_max = phymm_max;
	quota->pgc_count = pgc_count;
	quota->pgc_top = pgc_top;
	quota->sched_spent = task_schedule_time;
	quota->total_spent = time_now();
	return 0;
}


static int sys_pipe(int pipefd[2])
{
	int ret = fs_pipe(pipefd);

#ifdef __VERBOS_SYSCALL__
	klog("sys_pipe(pipefd[%d,%d]) = %d\n", pipefd[0], pipefd[1], ret);
#endif

	return ret;
}

static int sys_dup2(int oldfd, int newfd)
{
	int ret = -1;
#ifdef __VERBOS_SYSCALL__
	klog("sys_dup2(%d, %d)\n", oldfd, newfd);
#endif

	if (oldfd == -1 || newfd == -1) return -1;

	if (oldfd >= MAX_FD || newfd >= MAX_FD) return -1;

	if (oldfd == newfd)
	{
		return oldfd;
	}

	ret = fs_dup2(oldfd, newfd);
	if (ret == 0)
	{
		return newfd;
	}

	return ret;
}

static int sys_dup(int oldfd)
{
	int ret = -1;

	if (oldfd == -1 || oldfd >= MAX_FD) return -1;



	ret = fs_dup(oldfd);

#ifdef __VERBOS_SYSCALL__
	klog("sys_dup(%d) = %d\n", oldfd, ret);
#endif

	return ret;
}

static int sys_getrlimit(int resource, void *limit)
{
	return -1;
}

static int sys_kill(unsigned pid, int sig)
{
	return -1;
}

static int sys_unlink(const char *path)
{
	return -1;
	#if 0
	char *new = 0;
	struct stat s;
	int ret = -1;

#ifdef __VERBOS_SYSCALL__
	klog("sys_unlink(%s)\n", path);
#endif


	if (!path || !*path) return -EINVAL;

	new = kmalloc(64);
	resolve_path(path, new);

	if (fs_stat(new, &s) == -1)
	{
		kfree(new);
		return -ENOENT;
	}

	if (S_ISDIR(s.st_mode))
	{
		kfree(new);
		return -EISDIR;
	}

	ret = fs_delete(new); 
	kfree(new);
	if (ret)
		return 0;
	else
		return -1;
	#endif
}

