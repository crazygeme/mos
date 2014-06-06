#include <syscall/syscall.h>
#include <int/int.h>
#include <syscall/unistd.h>
#include <ps/ps.h>
#include <user/ps0.h>
#include <config.h>
#include <fs/namespace.h>
#include <int/timer.h>

  #define MAP_SHARED      0x01            /* Share changes */
  #define MAP_PRIVATE     0x02            /* Changes are private */
  #define MAP_TYPE        0x0f            /* Mask for type of mapping */
  #define MAP_FIXED       0x10            /* Interpret addr exactly */
  #define MAP_ANONYMOUS   0x20            /* don't use a file */
  #ifdef CONFIG_MMAP_ALLOW_UNINITIALIZED
  # define MAP_UNINITIALIZED 0x4000000    /* For anonymous mmap, memory could be uninitialized */
  #else
  # define MAP_UNINITIALIZED 0x0          /* Don't support this flag */
  #endif
  

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
static int sys_open(const char* name);
static int sys_close(unsigned fd);
static int sys_sched_yield();
static int sys_brk(unsigned top);
static int sys_chdir(const char *path);
static char *sys_getcwd(char *buf, unsigned size);
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
static int sys_fstat(int fd, struct stat* s);
static int sys_mprotect(void *addr, unsigned len, int prot);
static int sys_writev(int fildes, const struct iovec *iov, int iovcnt);
static long sys_personality(unsigned int personality);

static char* call_table_name[NR_syscalls] = {
	"test_call", 
    "sys_exit", "sys_fork", "sys_read", "sys_write", "sys_open",   // 1  ~ 5
    "sys_close", "sys_waitpid", "sys_creat", 0, 0,           // 6  ~ 10  
    "sys_execve", "sys_chdir", "time", 0, 0,           // 11 ~ 15
    0, 0, 0, 0, "sys_getpid",  // 16 ~ 20   
    0, 0, 0, "sys_getuid", 0,          // 21 ~ 25 
    0, 0, 0, 0, 0,          // 26 ~ 30 
    0, 0, 0, 0, 0,          // 31 ~ 35 
    0, 0, 0, "sys_mkdir", "sys_rmdir",          // 36 ~ 40 
    0, 0, 0, 0, "sys_brk",          // 41 ~ 45 
    0, "sys_getgid", 0, "sys_geteuid", "sys_getegid",          // 46 ~ 50 
    0, 0, 0, "sys_ioctl", 0,          // 51 ~ 55 
    0, 0, 0, 0, 0,          // 56 ~ 60 
    0, 0, 0, 0, 0,          // 61 ~ 65 
    0, 0, 0, 0, 0,          // 66 ~ 70 
    0, 0, 0, 0, 0,          // 71 ~ 75 
    0, 0, 0, 0, 0,          // 76 ~ 80 
    0, 0, 0, 0, 0,          // 81 ~ 85 
    0, 0, "sys_reboot", "sys_readdir", "sys_mmap",          // 86 ~ 90 
    0, 0, 0, 0, 0,          // 91 ~ 95 
    0, 0, 0, 0, 0,          // 96 ~ 100 
    0, 0, 0, 0, 0,          // 101 ~ 105
    "stat", 0, "fstat", 0, 0,          // 106 ~ 110 
    0, 0, 0, 0, 0,          // 111 ~ 115
    0, 0, 0, 0, 0,          // 116 ~ 120
    0, "sys_uname", 0, 0, "sys_mprotect",          // 121 ~ 125
    0, 0, 0, 0, 0,          // 126 ~ 130
    0, 0, 0, 0, 0,          // 131 ~ 135
    "sys_personality", 0, 0, 0, 0,          // 136 ~ 140
    0, 0, 0, 0, 0,          // 141 ~ 145
    "sys_writev", 0, 0, 0, 0,          // 146 ~ 150
    0, 0, 0, 0, 0,          // 151 ~ 155
    0, 0, "sys_sched_yield", 0, 0,          // 156 ~ 160
    0, 0, 0, 0, 0,          // 161 ~ 165
    0, 0, 0, 0, 0,          // 165 ~ 170
    0, 0, 0, 0, 0,          // 171 ~ 175
    0, 0, 0, 0, 0,          // 175 ~ 180
    0, 0, "sys_getcwd", 0, 0,          // 181 ~ 185
    0, 0, 0, 0, 0,          // 185 ~ 190
    0, 0,             // 191 ~ 194
};

typedef int (*syscall_fn)(unsigned ebx, unsigned ecx, unsigned edx);

static unsigned call_table[NR_syscalls] = {
	test_call, 
    sys_exit, sys_fork, sys_read, sys_write, sys_open,   // 1  ~ 5
    sys_close, sys_waitpid, sys_creat, 0, 0,           // 6  ~ 10  
    sys_execve, sys_chdir, time, 0, 0,           // 11 ~ 15
    0, 0, 0, 0, sys_getpid,  // 16 ~ 20   
    0, 0, 0, sys_getuid, 0,          // 21 ~ 25 
    0, 0, 0, 0, 0,          // 26 ~ 30 
    0, 0, 0, 0, 0,          // 31 ~ 35 
    0, 0, 0, sys_mkdir, sys_rmdir,          // 36 ~ 40 
    0, 0, 0, 0, sys_brk,          // 41 ~ 45 
    0, sys_getgid, 0, sys_geteuid, sys_getegid,          // 46 ~ 50 
    0, 0, 0, sys_ioctl, 0,          // 51 ~ 55 
    0, 0, 0, 0, 0,          // 56 ~ 60 
    0, 0, 0, 0, 0,          // 61 ~ 65 
    0, 0, 0, 0, 0,          // 66 ~ 70 
    0, 0, 0, 0, 0,          // 71 ~ 75 
    0, 0, 0, 0, 0,          // 76 ~ 80 
    0, 0, 0, 0, 0,          // 81 ~ 85 
    0, 0, sys_reboot, sys_readdir, sys_mmap,          // 86 ~ 90 
    0, 0, 0, 0, 0,          // 91 ~ 95 
    0, 0, 0, 0, 0,          // 96 ~ 100 
    0, 0, 0, 0, 0,          // 101 ~ 105
    fs_stat, 0, fs_fstat, 0, 0,          // 106 ~ 110 
    0, 0, 0, 0, 0,          // 111 ~ 115
    0, 0, 0, 0, 0,          // 116 ~ 120
    0, sys_uname, 0, 0, sys_mprotect,          // 121 ~ 125
    0, 0, 0, 0, 0,          // 126 ~ 130
    0, 0, 0, 0, 0,          // 131 ~ 135
    sys_personality, 0, 0, 0, 0,          // 136 ~ 140
    0, 0, 0, 0, 0,          // 141 ~ 145
    sys_writev, 0, 0, 0, 0,          // 146 ~ 150
    0, 0, 0, 0, 0,          // 151 ~ 155
    0, 0, sys_sched_yield, 0, 0,          // 156 ~ 160
    0, 0, 0, 0, 0,          // 161 ~ 165
    0, 0, 0, 0, 0,          // 165 ~ 170
    0, 0, 0, 0, 0,          // 171 ~ 175
    0, 0, 0, 0, 0,          // 175 ~ 180
    0, 0, sys_getcwd, 0, 0,          // 181 ~ 185
    0, 0, 0, 0, 0,          // 185 ~ 190
    0, 0             // 191 ~ 194
};

static int unhandled_syscall(unsigned callno)
{
	printk("unhandled syscall %d\n", callno);
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

	if (fd > MAX_FD)
		return -1;

	if (cur->fds[fd].flag & fd_flag_isdir)
		return -1;

	ino = cur->fds[fd].file;
	if ( ino == INODE_STD_OUT || ino == INODE_STD_ERR )
	{
		return -1;
	}
	else if ( ino == INODE_STD_IN )
	{
		if ( len < 1 )
		   return -1;

		*buf = kb_buf_get();
		return 1;
	}
	else
	{
		unsigned offset = cur->fds[fd].file_off;
		#ifdef __VERBOS_SYSCALL__
		printf("read(%d, %x, %x, %x) = ", fd, offset, buf, len);
		#endif
		len = fs_read(fd, offset, buf, len);
		#ifdef __VERBOS_SYSCALL__
		printf("%d\n", len);
		#endif
		offset += len;
		cur->fds[fd].file_off = offset;
	}

	return len;
}

static int sys_write(unsigned fd, char* buf, unsigned len)
{
	task_struct* cur = CURRENT_TASK();
	unsigned ino = 0;

	if (fd > MAX_FD)
		return -1;

	if (cur->fds[fd].flag & fd_flag_isdir)
		return -1;

	ino = cur->fds[fd].file;
	if ( ino == INODE_STD_OUT || ino == INODE_STD_ERR )
	{
		tty_write(buf, len);
	}
	else if ( ino == INODE_STD_IN )
	{
		return -1;
	}
	else
	{
		unsigned offset = cur->fds[fd].file_off;
		len = fs_write(fd, offset, buf, len);
		offset += len;
		cur->fds[fd].file_off = offset;
	}

	return len;
}

static int sys_ioctl(int fd, int request, char* buf)
{
	task_struct* cur = CURRENT_TASK();
	unsigned ino = 0;

	if (fd > MAX_FD)
		return -1;

	if (cur->fds[fd].flag & fd_flag_isdir)
		return -1;

	if (request > IOCTL_MAX)
		return -1;

	ino = cur->fds[fd].file;
	if ( ino == INODE_STD_OUT || ino == INODE_STD_ERR )
	{
		if (request == IOCTL_TTY)
		{
			return tty_ioctl(buf);
		}
	}
	else
	{
		return -1;
	}

	return 1;
}

static int sys_getpid()
{
    task_struct* cur = CURRENT_TASK();
    return cur->psid;
}

static int sys_uname(struct utsname* utname)
{
	strcpy(utname->machine, "qemu");
	strcpy(utname->nodename, "qemu-enum");
	strcpy(utname->release, "ender");
	strcpy(utname->sysname, "Mos");
	strcpy(utname->version, "0.1");
	return 1;
}

static int sys_sched_yield()
{
    task_sched();
    return 0;
}

static int sys_open(const char* name)
{
	return fs_open(name);
}

static int sys_close(unsigned fd)
{
	fs_close(fd);
	return 1;
}

static int sys_brk(unsigned top)
{
	task_struct* task = CURRENT_TASK();
	unsigned size = 0;
	unsigned pages = 0;

    if (task->user.heap_top == USER_HEAP_BEGIN) {
        mm_add_dynamic_map(task->user.heap_top, 0, PAGE_ENTRY_USER_DATA);
		task->user.heap_top += PAGE_SIZE;
    }

	#ifdef __VERBOS_SYSCALL__
	printf("brk: top %x, ", top);
	#endif
	if ( top == 0 )
	{
		#ifdef __VERBOS_SYSCALL__
		printf("ret %x\n", task->user.heap_top);
		#endif

		return task->user.heap_top;
	}
	else if ( top > task->user.heap_top )
	{
		int i = 0;
		size = top - task->user.heap_top;
		pages = (size-1) / PAGE_SIZE + 1;

		for (i = 0; i < pages; i++)
		{
			unsigned vir = task->user.heap_top + i * PAGE_SIZE;
			mm_add_dynamic_map(vir, 0, PAGE_ENTRY_USER_DATA);
		}

		top = task->user.heap_top + pages * PAGE_SIZE;
		task->user.heap_top = top;
		#ifdef __VERBOS_SYSCALL__
		printf("ret %x\n", top);
		#endif
		return top;
	}else{
		// FIXME
		// free heap
	}

	return 0;
}

static int _chdir(const char* cwd, const char *path)
{
	struct stat s;

	if (!path)
	   return 0;

	if (!*path)
		return 1;

	if (!strcmp(path, "."))
		return 1;

	if (!strcmp(path, "..")){
		char* tmp;
		tmp = strrchr(cwd, '/');
		if (tmp)
			*tmp = '\0';
		strcpy(cwd, cwd);
		return 1;
	}

	strcat(cwd, "/");
	strcat(cwd, path);
	if(fs_stat(cwd, &s) == -1)
		return 0;

	if (!S_ISDIR(s.st_mode))
		return 0;

	strcpy(cwd, cwd);
		return 1;
}

static int sys_chdir(const char* path)
{
	char name[256] = {0};
	char* slash, *tmp;
	char cwd[256] = {0};
	int ret = 1;
	task_struct* cur = CURRENT_TASK();
	
	

	if (!path || !*path)
	   return 0;

	if (*path != '/')
		strcpy(cwd, cur->cwd);

	strcpy(name, path);
	slash = strchr(name, '/');
	tmp = name;
	while (slash)
	{
		*slash = '\0';
		ret = _chdir(cwd, tmp);
		if (!ret)
			return 0;

		slash++;
		tmp = slash;
		slash = strchr(tmp, '/');
	}

	ret = _chdir(cwd, tmp);
	if (!ret)
		return 0;

	strcpy(cur->cwd, cwd);
	return 1;
}

static char *sys_getcwd(char *buf, unsigned size)
{
	task_struct* cur = CURRENT_TASK();

	if (!cur->cwd[0])
	    strcpy(buf, "/");
	else
		strcpy(buf, cur->cwd);

	return buf;
}

static int sys_creat(const char* path, unsigned mode)
{
	struct stat s;
	if (fs_stat(path, &s) != -1)
		return 0;
	return fs_create(path,mode);
}

static int sys_mkdir(const char* path, unsigned mode)
{
	mode |= S_IFDIR;
	return sys_creat(path,mode);
}

static int sys_rmdir(const char* path)
{
	struct stat s;
	if (fs_stat(path, &s) == -1)
		return 0;

	if (!S_ISDIR(s.st_mode))
		return 0;

	if (s.st_size)
		return 0;

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

static int sys_mmap(struct mmap_arg_struct32* arg)
{
	unsigned addr = arg->addr & PAGE_SIZE_MASK;
	unsigned last_addr = (arg->addr + arg->len-1) & PAGE_SIZE_MASK;
	unsigned i = addr, map_ret;
	unsigned page_count = (last_addr - addr) / PAGE_SIZE + 1;
	unsigned read_addr = addr;

    if (arg->addr == 0) {
        addr = vm_get_usr_zone(page_count);
		last_addr = addr + (page_count-1)*PAGE_SIZE;
		read_addr = addr;
    }

	for (i = addr; i <= last_addr; i+=PAGE_SIZE){
		map_ret = mm_add_dynamic_map(i, 0, PAGE_ENTRY_USER_DATA);
		if (map_ret > 0 && arg->fd > 0)
		{
			memset(0, i, PAGE_SIZE);
		}
	}

	#ifdef __VERBOS_SYSCALL__
	printf("mmap: fd %d, addr %x, offset %x, len %x at addr %x\n",
		   arg->fd, arg->addr, arg->offset, arg->len, addr);
	printf("\t page count %d, first %x, last %x\n", page_count, addr, last_addr+PAGE_SIZE);
	#endif

	if ((map_ret > 0) || (arg->flags & MAP_FIXED)){
		if (arg->fd > 0 && arg->fd < MAX_FD){
			unsigned ret = 0;
			memset(read_addr, 0, arg->len);
			if (!debug)
			{
				ret = fs_read(arg->fd, arg->offset, read_addr, arg->len);
			}
			#ifdef __VERBOS_SYSCALL__
			printf("\t read(%d, %x, %x %x) = %x\n", arg->fd, arg->offset, read_addr, arg->len, ret);
			#endif
		}else{
			memset(read_addr, 0, arg->len);
		}
	}

	return addr;
}

static int sys_mprotect(void *addr, unsigned len, int prot)
{
	#ifdef __VERBOS_SYSCALL__
	printf("mprotect: addr %x, len %x, prot %x\n", addr, len, prot);
	#endif
	return 0;
}

static int sys_writev(int fildes, const struct iovec *iov, int iovcnt)
{
	int i = 0;
	unsigned total = 0;

    for (i = 0; i < iovcnt; i++) {
		total += sys_write(fildes, iov[i].iov_base, iov[i].iov_len);
        
    }
	return total;
}

static long sys_personality(unsigned int personality)
{
	return PER_LINUX32_3GB;
}



