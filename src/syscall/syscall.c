#include <syscall.h>
#include <int.h>
#include <unistd.h>
#include <ps.h>
#include <ps0.h>
#include <config.h>
#include <timer.h>
#include <klib.h>
#include <rbtree.h>
#include <fcntl.h>
#include <errno.h>
#include <3rdparty/lwext4/include/ext4.h>
#include <include/fs.h>
#include <select.h>


struct utimbuf
{
    unsigned actime;       /* access time */
    unsigned modtime;      /* modification time */
};

struct mmap_arg_struct32
{
    unsigned int addr;
    unsigned int len;
    unsigned int prot;
    unsigned int flags;
    int fd;
    unsigned int offset;
};

struct iovec
{
    char   *iov_base;  /* Base address. */
    unsigned iov_len;    /* Length. */
};

enum
{
    UNAME26 = 0x0020000,
    ADDR_NO_RANDOMIZE = 0x0040000,      /* disable randomization of VA space */
    FDPIC_FUNCPTRS = 0x0080000,      /* userspace function ptrs point to descriptors
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
#define PER_CLEAR_ON_SETID (READ_IMPLIES_EXEC  | \
                              ADDR_NO_RANDOMIZE  | \
                              ADDR_COMPAT_LAYOUT | \
                              MMAP_PAGE_ZERO)

enum
{
    PER_LINUX = 0x0000,
    PER_LINUX_32BIT = 0x0000 | ADDR_LIMIT_32BIT,
    PER_LINUX_FDPIC = 0x0000 | FDPIC_FUNCPTRS,
    PER_SVR4 = 0x0001 | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
    PER_SVR3 = 0x0002 | STICKY_TIMEOUTS | SHORT_INODE,
    PER_SCOSVR3 = 0x0003 | STICKY_TIMEOUTS |
    WHOLE_SECONDS | SHORT_INODE,
    PER_OSR5 = 0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS,
    PER_WYSEV386 = 0x0004 | STICKY_TIMEOUTS | SHORT_INODE,
    PER_ISCR4 = 0x0005 | STICKY_TIMEOUTS,
    PER_BSD = 0x0006,
    PER_SUNOS = 0x0006 | STICKY_TIMEOUTS,
    PER_XENIX = 0x0007 | STICKY_TIMEOUTS | SHORT_INODE,
    PER_LINUX32 = 0x0008,
    PER_LINUX32_3GB = 0x0008 | ADDR_LIMIT_3GB,
    PER_IRIX32 = 0x0009 | STICKY_TIMEOUTS,/* IRIX5 32-bit */
    PER_IRIXN32 = 0x000a | STICKY_TIMEOUTS,/* IRIX6 new 32-bit */
    PER_IRIX64 = 0x000b | STICKY_TIMEOUTS,/* IRIX6 64-bit */
    PER_RISCOS = 0x000c,
    PER_SOLARIS = 0x000d | STICKY_TIMEOUTS,
    PER_UW7 = 0x000e | STICKY_TIMEOUTS | MMAP_PAGE_ZERO,
    PER_OSF4 = 0x000f,                  /* OSF/1 v4 */
    PER_HPUX = 0x0010,
    PER_MASK = 0x00ff,
};


static int test_call(unsigned arg0, unsigned arg1, unsigned arg2);
static int sys_read(int fd, char* buf, unsigned len);
static int sys_write(int fd, char* buf, unsigned len);
static int sys_getpid();
static int sys_uname(struct utsname* utname);
static int sys_open(const char* name, int flags, char* mode);
static int sys_close(unsigned fd);
static int sys_sched_yield();
static int sys_brk(unsigned top);
static int sys_chdir(const char *path);
static int sys_ioctl(int d, int request, char* buf);
static int sys_creat(const char* path, unsigned mode);
static int sys_mkdir(const char* path, unsigned mode);
static int sys_rmdir(const char* path);
static int sys_readdir(unsigned fd, struct old_linux_dirent *dirp, unsigned count);
static int sys_reboot(unsigned cmd);
static int sys_getuid();
static int sys_getgid();
static int sys_geteuid();
static int sys_getegid();
static int sys_stat(const char *pathname, struct stat *buf);
static int sys_mmap(struct mmap_arg_struct32* arg);
static int sys_mprotect(void *addr, unsigned len, int prot);
static int sys_readv(int fildes, const struct iovec *iov, int iovcnt);
static int sys_writev(int fildes, const struct iovec *iov, int iovcnt);
static long sys_personality(unsigned int personality);
static int sys_fcntl(int fd, int cmd, int arg);
static int sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count);
static int sys_lstat64(const char* path, struct stat64* s);
static int sys_fstat(int fd, struct stat* s);
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
static int sys_time(unsigned* t);
static int resolve_path(const char* old, char* new);
static int sys_access(const char* path, int mode);
static int do_stat(const char* func, const char *_name, struct stat *buf, int follow_link);
static int sys_lseek(int fd, unsigned offset, int whence);
static int sys_llseek(int fd, unsigned offset_high,
                   unsigned offset_low, uint64_t *result,
                   unsigned int whence);
static int sys_select(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, const struct timespec *timeout);

static int sys_newselect(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, const struct timespec *timeout,
                   void *sigmask);

typedef int(*syscall_fn)(unsigned ebx, unsigned ecx, unsigned edx, unsigned esi, unsigned edi);

static unsigned call_table[NR_syscalls] = {
    test_call,
    sys_exit, sys_fork, sys_read, sys_write, sys_open,   // 1  ~ 5
    sys_close, sys_waitpid, sys_creat, 0, sys_unlink,           // 6  ~ 10  
    sys_execve, sys_chdir, sys_time, 0, 0,           // 11 ~ 15
    0, 0, 0, sys_lseek, sys_getpid,  // 16 ~ 20   
    0, 0, 0, sys_getuid, 0,          // 21 ~ 25 
    0, 0, 0, sys_pause, sys_utime,          // 26 ~ 30 
    0, 0, sys_access, 0, 0,          // 31 ~ 35
    0, sys_kill, 0, sys_mkdir, sys_rmdir,          // 36 ~ 40 
    sys_dup, sys_pipe, 0, 0, sys_brk,          // 41 ~ 45 
    0, sys_getgid, 0, sys_geteuid, sys_getegid,          // 46 ~ 50 
    0, 0, 0, sys_ioctl, sys_fcntl,          // 51 ~ 55 
    0, sys_setpgid, 0, 0, 0,          // 56 ~ 60 
    0, 0, sys_dup2, sys_getppid, sys_getpgrp,          // 61 ~ 65 
    0, 0, 0, 0, 0,          // 66 ~ 70 
    0, 0, 0, 0, 0,          // 71 ~ 75 
    sys_getrlimit, 0, 0, 0, 0,          // 76 ~ 80 
    0, sys_select, 0, 0, 0,          // 81 ~ 85 
    0, 0, sys_reboot, sys_readdir, sys_mmap,          // 86 ~ 90 
    sys_munmap, 0, 0, 0, 0,          // 91 ~ 95 
    0, 0, 0, 0, 0,          // 96 ~ 100 
    0, sys_socketcall, 0, 0, 0,          // 101 ~ 105
    sys_stat, 0, sys_fstat, 0, 0,          // 106 ~ 110 
    0, 0, 0, sys_wait4, 0,          // 111 ~ 115
    0, 0, 0, 0, 0,          // 116 ~ 120
    0, sys_uname, 0, 0, sys_mprotect,          // 121 ~ 125
    0, 0, 0, 0, 0,          // 126 ~ 130
    0, 0, 0, 0, 0,          // 131 ~ 135
    sys_personality, 0, 0, 0, sys_llseek,          // 136 ~ 140
    sys_getdents, sys_newselect, 0, 0, sys_readv,          // 141 ~ 145
    sys_writev, 0, 0, 0, 0,          // 146 ~ 150
    0, 0, 0, 0, 0,          // 151 ~ 155
    0, 0, sys_sched_yield, 0, 0,          // 156 ~ 160
    0, 0, 0, 0, 0,          // 161 ~ 165
    0, 0, 0, 0, 0,          // 165 ~ 170
    0, 0, 0, sys_sigaction, sys_sigprocmask,          // 171 ~ 175
    0, 0, 0, 0, 0,          // 175 ~ 180
    0, 0, sys_getcwd, 0, 0,          // 181 ~ 185
    0, 0, 0, 0, sys_vfork,          // 185 ~ 190
    0, 0, 0, 0, 0,            // 191 ~ 195
    sys_lstat64, sys_fstat64, sys_quota            // 196 ~ 198
};

static int unhandled_syscall(unsigned callno)
{
    if (TestControl.verbos)
        klog("%d: unhandled syscall %d\n", CURRENT_TASK()->psid, callno);
    return -1;
}



static void syscall_process(intr_frame* frame)
{
    syscall_fn fn = call_table[frame->eax];
    int ret = 0;
    if (!fn)
    {
        ret = unhandled_syscall(frame->eax);
    }
    else
    {
        ret = (unsigned)fn(frame->ebx, frame->ecx, frame->edx, frame->esi, frame->edi);
    }

    frame->eax = ret;
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

static int sys_read(int fd, char* buf, unsigned len)
{
    task_struct* cur = CURRENT_TASK();
    int i = 0, pri_len = 0;
    int ret = 0;
    if (fd < 0 || fd >= MAX_FD)
        return -1;

    if (cur->fds[fd].used == 0)
        return -1;

    if ( S_ISDIR(cur->fds[fd].fp->mode))
        return -1;

    if (TestControl.verbos) {
        klog("%d: read(%d, \"", CURRENT_TASK()->psid, fd);
    }

    unsigned offset = cur->fds[fd].file_off;
    ret = fs_read(fd, offset, buf, len);
    offset += ret;
    cur->fds[fd].file_off = offset;


    if (TestControl.verbos) {
        pri_len = (ret > 5) ? 5 : ret;
        for (i = 0; i < pri_len; i++) {
            if (isprint(buf[i]))
                klog_printf("%c", buf[i]);
            else
                klog_printf("\\%x", buf[i]);
        }
        klog_printf("\", %d) = %d\n", len, ret);
    }

    return ret;
}

static int sys_write(int fd, char *buf, unsigned len)
{
    task_struct *cur = CURRENT_TASK();
    unsigned _len;
    if (TestControl.verbos) {
        char *tmp;
        int pri_len = len > 5 ? 5 : len;
        tmp = kmalloc(6);
        memset(tmp, 0, 6);
        memcpy(tmp, buf, pri_len);
        klog("%d: write(%d, \"%s\", %d) ", CURRENT_TASK()->psid, fd, tmp, len);
        kfree(tmp);
    }

    if (fd < 0 || fd >= MAX_FD)
    {
        return -1;
    }

    if (cur->fds[fd].used == 0)
    {
        if (TestControl.verbos) {
            klog_printf("ret -1\n");
        }
        return -1;
    }

    if ( S_ISDIR(cur->fds[fd].fp->mode))
    {
        if (TestControl.verbos) {
            klog_printf("ret -1\n");
        }
        return -1;
    }

    unsigned offset = cur->fds[fd].file_off;
    _len = fs_write(fd, offset, buf, len);
    offset += _len;
    cur->fds[fd].file_off = offset;

    if (TestControl.verbos) {
        klog_printf("ret %d\n", _len);
    }
    return _len;
}

static int sys_ioctl(int fd, int request, char *buf)
{
    return fs_ioctl(fd, request, buf);
}

static int sys_getpid()
{
    task_struct *cur = CURRENT_TASK();
    if (TestControl.verbos) {
        klog("%d: getpid() = %d\n", CURRENT_TASK()->psid, 0);
    }
    return cur->psid;
}

static int sys_uname(struct utsname *utname)
{
    if (TestControl.verbos) {
        klog("%d: uname\n", CURRENT_TASK()->psid);
    }
    strcpy(utname->machine, "i386");
    strcpy(utname->nodename, "qemu-enum");
    strcpy(utname->release, "0.5-generic");
    strcpy(utname->sysname, "Mos");
    strcpy(utname->version, "Mos Wed Feb 19 10:56:00 UTC 2015");
    strcpy(utname->domain, "Ender");
    return 1;
}

static int sys_sched_yield()
{
    if (TestControl.verbos) {
        klog("%d: yield\n", CURRENT_TASK()->psid);
    }
    task_sched();
    return 0;
}

static int sys_open(const char *_name, int flags, char* mode)
{
    char *name = name_get();
    int fd;
    resolve_path(_name, name);


    if (TestControl.verbos) {
        klog("%d: open(%s, %x, %x)", CURRENT_TASK()->psid, name, flags, mode);
    }

    fd = fs_open(name, flags, mode);

    if (TestControl.verbos) {
        klog_printf("ret %d\n", fd);
    }
    name_put(name);
    return fd;
}

static int sys_close(unsigned fd)
{
    if (TestControl.verbos) {
        klog("%d: close(%d)\n", CURRENT_TASK()->psid, fd);
    }
    return fs_close(fd);
}

static int sys_brk(unsigned _top)
{
    task_struct *task = CURRENT_TASK();
    unsigned size = 0;
    unsigned pages = 0;
    unsigned ret,top;
    top = _top;
    if (task->user.heap_top == USER_HEAP_BEGIN)
    {
        mm_add_dynamic_map(task->user.heap_top, 0, PAGE_ENTRY_USER_DATA);
        REFRESH_CACHE();
        task->user.heap_top += PAGE_SIZE;
    }


    if (top == 0)
    {
        ret = task->user.heap_top;
        goto done;
    }
    else if (top >= USER_HEAP_END)
    {
        ret =  task->user.heap_top;
        goto done;
    }
    else if (top > task->user.heap_top)
    {
        int i = 0;
        size = top - task->user.heap_top;
        pages = (size - 1) / PAGE_SIZE + 1;

        for (i = 0; i < pages; i++)
        {
            unsigned vir = task->user.heap_top + i * PAGE_SIZE;
            mm_add_dynamic_map(vir, 0, PAGE_ENTRY_USER_DATA);
        }
        REFRESH_CACHE();
        top = task->user.heap_top + pages * PAGE_SIZE;
        task->user.heap_top = top;
        ret =  top;
        goto done;
    }
    else
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
        REFRESH_CACHE();
        task->user.heap_top = top;
        ret = top;
        goto done;
    }
done:
    if (TestControl.verbos) {
            klog("%d: brk(%x) = %x\n", CURRENT_TASK()->psid, _top, ret);
    }
    return ret;
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
    if (cwd[strlen(cwd)-1] != '/')
        strcat(cwd, "/");
    strcat(cwd, path);
    strcat(cwd, "/");
    if (do_stat(__func__, cwd, &s, 1) == -1) return 0;

    if (!S_ISDIR(s.st_mode)) return 0;

    strcpy(cwd, cwd);
    return 1;
}

static int sys_chdir(const char *path)
{
    char *name = name_get();
    char *slash, *tmp;
    char *cwd = name_get();
    int ret = 1;
    task_struct *cur = CURRENT_TASK();
    int len;
    if (!path || !*path) return 0;

    if (TestControl.verbos) {
        klog("%d: chdir %s\n", CURRENT_TASK()->psid, path);
    }

    if (*path != '/')
        strcpy(cwd, cur->cwd);
    else{
        len = strlen(path);
        strcpy(cur->cwd, path);
        if (path[len-1] != '/')
            strcat(cur->cwd, "/");
        ret = 0;
        goto done;
    }

    strcpy(name, path);
    slash = strchr(name, '/');
    tmp = name;
    while (slash)
    {
        *slash = '\0';
        ret = _chdir(cwd, tmp);
        if (!ret) return -1;

        slash++;
        tmp = slash;
        slash = strchr(tmp, '/');
    }

    ret = _chdir(cwd, tmp);
    if (!ret){
        ret = -1;
        goto done;
    }

    strcpy(cur->cwd, cwd);
done:
    name_put(name);
    name_put(cwd);
    return ret;
}





static int sys_creat(const char *path, unsigned mode)
{
    if (TestControl.verbos) {
        klog("%d: creat(%s, %d)\n", CURRENT_TASK()->psid, path, mode);
    }
    return -1;
}

static int sys_mkdir(const char *path, unsigned mode)
{
    char* name = name_get();
    int ret;
    resolve_path(path, name);
    ret = ext4_dir_mk(name);
    if (TestControl.verbos) {
        klog("%d: %dmkdir(%s, %d)\n", CURRENT_TASK()->psid, path, mode);
    }
    name_put(name);
    return ret;
}

static int sys_rmdir(const char *path)
{
    char* name = name_get();
    int ret;
    resolve_path(path, name);
    ret = ext4_dir_rm(name);
    if (TestControl.verbos) {
        klog("%d: %drmdir(%s)\n", CURRENT_TASK()->psid, name);
    }
    name_put(name);
    return ret;
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
    if (TestControl.verbos) {
        klog("%d: getuid\n", CURRENT_TASK()->psid);
    }
    return 0;
}

static int sys_getgid()
{
    if (TestControl.verbos) {
        klog("%d: getgid\n", CURRENT_TASK()->psid);
    }
    return 0;
}

static int sys_geteuid()
{
    if (TestControl.verbos) {
        klog("%d: geteuid\n", CURRENT_TASK()->psid);
    }
    return 0;
}

static int sys_getegid()
{
    if (TestControl.verbos) {
        klog("%d: getegid\n", CURRENT_TASK()->psid);
    }
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

    if (TestControl.verbos) {
        klog("%d: mmap: fd %d, addr %x, offset %x, len %x at addr %x\n",
             CURRENT_TASK()->psid,
             arg->fd, arg->addr, arg->offset, arg->len, vir);
    }

    return vir;
}

static int sys_munmap(void *addr, unsigned length)
{
    if (TestControl.verbos) {
        klog("%d: munmap (%x, %x)\n", CURRENT_TASK()->psid, addr, length);
    }
    return do_munmap(addr, length);
}

static int sys_mprotect(void *addr, unsigned len, int prot)
{
    if (TestControl.verbos) {
        klog("%d: mprotect: addr %x, len %x, prot %x\n", CURRENT_TASK()->psid, addr, len, prot);
    }
    return 0;
}

static int sys_readv(int fildes, const struct iovec *iov, int iovcnt)
{
    int i = 0;
    unsigned total = 0;
    for (i = 0; i < iovcnt; i++)
    {
        total += iov[i].iov_len;
        memset(iov[i].iov_base, 0, iov[i].iov_len);
    }

    return total;
}

static int sys_writev(int fildes, const struct iovec *iov, int iovcnt)
{
    if (TestControl.verbos) {
        klog("%d: writev: fd %d\n", CURRENT_TASK()->psid, fildes);
    }
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
    if (TestControl.verbos) {
        klog("%d: personality\n", CURRENT_TASK()->psid);
    }
    return PER_LINUX32_3GB;
}

static int sys_fcntl(int fd, int cmd, int arg)
{
    task_struct* cur = CURRENT_TASK();
    int ret = 0;
    if (fd < 0 || fd >= MAX_FD)
        return -ENOENT;

    if (cur->fds[fd].used == 0)
        return -ENOENT;

    switch(cmd) {
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

 static int sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count)
 {
    ext4_direntry* entry = NULL;
    int retcount;
    int len;
    int ret = 0;
    int cur_pos = 0;
    struct linux_dirent *prev;
    struct stat s;
    filep fp;
    ext4_dir* dir;


     if (TestControl.verbos) {
         klog("%d: getdents(%d, %x, %d)", CURRENT_TASK()->psid, fd, dirp, count);
     }

    if (fd < 0 || fd >= MAX_FD)
    {
        if (TestControl.verbos) {
            klog_printf(" = %d\n", -9);
        }
        return -ENOENT;
    }

    if (fs_fstat(fd, &s) != EOK)
        return -ENOENT;
    
    if (!S_ISDIR(s.st_mode))
        return -EISDIR;

    fp = CURRENT_TASK()->fds[fd].fp;

    if (count < sizeof(struct linux_dirent))
    {
        if (TestControl.verbos) {
            klog_printf(" = %d\n", -22);
        }
        return -22;
    }
    retcount = 0;
    prev = 0;
    dir = fp->inode;
    while (count > 0)
    {
        entry = ext4_dir_entry_next(fp->inode);
        //ret = fs_read(fd, 0, entry, sizeof(*entry));
        if (entry == NULL)
        {
            if (prev){
                prev->d_off = retcount;
            }
            break;
        }
        //entry->name[entry->name_length] = '\0';
        len = ROUND_UP(NAME_OFFSET(dirp) + strlen(entry->name) + 1);
        if (count < len)
        {
            if (prev){
                prev->d_off = retcount;
            }
            break;
        }
        memset(dirp, 0, len);
        dirp->d_ino = entry->inode;
        strncpy(dirp->d_name, entry->name, entry->name_length);
        dirp->d_reclen = ROUND_UP(NAME_OFFSET(dirp) + strlen(dirp->d_name) + 1);
        cur_pos += dirp->d_reclen;
        dirp->d_off = cur_pos;
        retcount += dirp->d_reclen;
        count -= dirp->d_reclen;
        prev = dirp;
        dirp = (char *)dirp + dirp->d_reclen;
    }
    
     if (TestControl.verbos) {
         klog_printf(" = %d\n", retcount);
     }
    return retcount;
 
 }

static int sys_fstat(int fd, struct stat *s)
{
    int ret = fs_fstat(fd, s);
    if (TestControl.verbos) {
        klog("%d: sys_fstat(%d, %x) size %d isdir %d blksize %d\n",
             CURRENT_TASK()->psid,
             fd, s, s->st_size, S_ISDIR(s->st_mode), s->st_blksize);
    }
    return ret;
}

static int sys_fstat64(int fd, struct stat64 *s) {
    // FIXME
    // rewrite it all please
    task_struct *cur = CURRENT_TASK();
    struct stat s32;

    if (fd < 0 || fd >= MAX_FD) return -1;

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

    if (TestControl.verbos) {
        klog("%d: fstat64(%d, %x) size %d blocks %d blksize %d\n",
             CURRENT_TASK()->psid,
             fd, s, s32.st_size, s32.st_blocks, s32.st_blksize);
    }

    return 0;
}


static int sys_lstat64(const char *path, struct stat64 *s)
{
    struct stat s32;
    char* name = name_get();
    int ret;
    resolve_path(path, name);
    ret = do_stat(__func__, name, &s32, 0);
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
    name_put(name);
    return ret;
}

static int sys_getppid()
{
    task_struct *cur = CURRENT_TASK();
    if (TestControl.verbos) {
        klog("%d: getppid\n", CURRENT_TASK()->psid);
    }
    return cur->parent;
}

static int sys_getpgrp(unsigned pid)
{
    if (TestControl.verbos) {
        klog("%d: getpgrp\n", CURRENT_TASK()->psid);
    }
    // FIXME
    return 0;
}

static int sys_setpgid(unsigned pid, unsigned pgid)
{
    if (TestControl.verbos) {
        klog("%d: setgpid\n", CURRENT_TASK()->psid);
    }
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
    }
    else if (pid < -1)
    {
        // FIXME
        // wait on group id
        return 0;
    }
    else
    {
        return sys_waitpid(pid, status, 0);
    }
}

static int sys_socketcall(int call, unsigned long *args)
{
    // FIXME
    // no socket at all right now
    if (TestControl.verbos) {
        klog("%d: socketcall\n", CURRENT_TASK()->psid);
    }
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
    if (TestControl.verbos) {
        klog("%d: pause\n", CURRENT_TASK()->psid);
    }
    __asm__("hlt");
    return -1;
}

static int resolve_path(const char *old, char *new)
{
    char* r;
    int len = strlen(old);
    if (!old || !*old)
        return -1;

    if (!strcmp(old, "."))
    {
        sys_getcwd(new, MAX_PATH);
        return 0;
    }

    if (!strcmp(old, ".."))
    {
        sys_getcwd(new, MAX_PATH);
        r = strrchr(new, '/');
        *r = '\0';
        if (*new == '\0') {
            *new++ = '/';
            *new++ = '\0';
        }
        return 0;
    }

    if (old[0] == '/')
    {
        strcpy(new, old);
        if (len >= 2 && new[len-1] == '.' && new[len-2] == '.')
        {
            r = strrchr(new, '/');
            if (!r) return -1;
            r = strrchr(r-1, '/');
            if (!r) return -1;
            r++;
            *r = '\0';
        }
        else if (len >= 1 && new[len-1] == '.')
        {
            new[len-1] = '\0';
        }
        return 0;
    }
    else if (len > 1 && old[0] == '.' && old[1] == '/')
    { 
        old += 2;
    }

    sys_getcwd(new, MAX_PATH);
    strcat(new, old);
    return 0;
}

static int sys_utime(const char *filename, const struct utimbuf *times)
{
    if (TestControl.verbos) {
        klog("%d: utime\n", CURRENT_TASK()->psid);
    }
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
extern unsigned page_fault_count;
extern unsigned page_falut_total_time;
static int sys_quota(struct krnquota *quota)
{
    quota->heap_cur = heap_quota;
    quota->heap_wm = heap_quota_high;
    quota->heap_top = cur_block_top;
    quota->phymm_max = phymm_max;
    quota->pgc_count = pgc_count;
    quota->pgc_top = pgc_top;
    quota->sched_spent = task_schedule_time;
    quota->total_spent = time_now();
    //printk("page fault count %d, total time %d ms\n", page_fault_count, page_falut_total_time);
    //quota->page_fault = page_fault_count;
    return 0;
}


static int sys_pipe(int pipefd[2])
{
    int ret = fs_pipe(pipefd);

    if (TestControl.verbos) {
        klog("%d: pipe(pipefd[%d,%d]) = %d\n", CURRENT_TASK()->psid, pipefd[0], pipefd[1], ret);
    }

    return ret;
}

static int sys_dup2(int oldfd, int newfd)
{
    int ret = -1;
    if (TestControl.verbos) {
        klog("%d: dup2(%d, %d)\n", CURRENT_TASK()->psid, oldfd, newfd);
    }

    if (oldfd == -1 || newfd == -1) return -1;

    if (oldfd >= MAX_FD || newfd >= MAX_FD) return -1;

    if (oldfd == newfd)
    {
        return -1;
    }

    ret = fs_dup2(oldfd, newfd);

    return ret;
}

static int sys_dup(int oldfd)
{
    int ret = -1;

    if (oldfd == -1 || oldfd >= MAX_FD) return -1;



    ret = fs_dup(oldfd);

    if (TestControl.verbos) {
        klog("%d: dup(%d) = %d\n", CURRENT_TASK()->psid, oldfd, ret);
    }

    return ret;
}

static int sys_getrlimit(int resource, void *limit)
{
    if (TestControl.verbos) {
        klog("%d: getrlimit\n", CURRENT_TASK()->psid);
    }
    return -1;
}

static int sys_kill(unsigned pid, int sig)
{
    if (TestControl.verbos) {
        klog("%d: kill\n", CURRENT_TASK()->psid);
    }
    return -1;
}

static int sys_unlink(const char *_name)
{
    char *name = name_get();
    struct stat s;
    int ret = -1;

    resolve_path(_name, name);
    ret = do_stat(__func__, name, &s, 0);
    if (ret != EOK)
        goto done;

    if (S_ISDIR(s.st_mode)){
        ret = -EISDIR;
        goto done;
    }

    ret = ext4_fremove(name);
done:
    name_put(name);
    if (TestControl.verbos) {
       klog("%d: unlink(%s) = %d\n", CURRENT_TASK()->psid, name, ret);
    }
    return ret;
}

static int sys_time(unsigned* t)
{
    if (!t)
        return -1;

    *t = (unsigned)time(0).time;
    return 0;
}

static int sys_readdir(unsigned fd, struct old_linux_dirent *dirp, unsigned count)
{
    int ret = -1;
    if (TestControl.verbos) {
        klog("%d: readdir(%d, %x, %d) = %d\n", CURRENT_TASK()->psid, fd, dirp, count, ret);
    }
    return sys_getdents(fd, dirp, count);
}

static int format_modes(unsigned mode, char* str)
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

static int do_stat(const char* func, const char *name, struct stat *buf, int follow_link)
{
    int ret = -1;
    filep fp = NULL;
    char modes[11];
    fp = fs_open_file(name, 0, follow_link);
    if (fp == NULL)
        goto done;
    if (!fp->op.stat)
        goto done;
    ret = fp->op.stat(fp->inode, buf);
    if (TestControl.verbos) {
        format_modes(buf->st_mode, modes);
        klog("%d: [%s](%s, %x) = %d, %s, len=%d, blocks = %d\n", CURRENT_TASK()->psid, func, name, buf, ret, modes, buf->st_size, buf->st_blocks);
    }
    
done:
    if (fp){
        fs_destroy(fp);
    }
    return ret;
}

static int sys_stat(const char *_name, struct stat *buf)
{
    char* name = name_get();
    int ret;
    resolve_path(_name, name);
    ret = do_stat(__func__, name, buf, 1);
    name_put(name);
    return ret;
}

static int sys_access(const char* path, int mode)
{
    // FIXME: no user currently
    struct stat s;
    char* name = name_get();
    int ret = -EACCES;

    resolve_path(path, name);
    ret = do_stat(__func__, name, &s, 1);

    if (ret != EOK) {
        ret = -ENOENT;
        goto done;
    }

    if (mode == F_OK)
        goto done;

    ret = EOK;
    if (mode & R_OK)
        ret |=  ((s.st_mode & S_IRUSR) == S_IRUSR) ? EOK : (-EACCES);

    if (mode & W_OK)
        ret |= ((s.st_mode & S_IWUSR) == S_IWUSR) ? EOK : (-EACCES);

    if (mode & X_OK)
        ret |= ((s.st_mode & S_IXUSR) == S_IXUSR) ? EOK : (-EACCES);

done:
    name_put(name);
    if (TestControl.verbos) {
        klog("%d: access(%s, %x) = %d\n", CURRENT_TASK()->psid, path, mode, ret);
    }

    return ret;
}

static int sys_lseek(int fd, unsigned offset, int whence)
{
    int ret;
    ret =  fs_seek(fd, offset, whence);
    if (TestControl.verbos) {
        klog("%d: lseek(%d, %d, %d) = %d\n", CURRENT_TASK()->psid, fd, offset, whence, ret);
    }
    return ret;
}

static int sys_llseek(int fd, unsigned offset_high,
                   unsigned offset_low, uint64_t *result,
                   unsigned whence)
{
    int ret;
    ret = fs_llseek(fd, offset_high, offset_low, result, whence);
    if (TestControl.verbos) {
        klog("%d: llseek(%d, %d, %d, %x, %d) = %d\n", CURRENT_TASK()->psid, fd, offset_high, offset_low, result, whence, ret);
    }
    return ret;
}

static int sys_select(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, const struct timespec *timeout)
{
    int ret = do_select(nfds, readfds, writefds, exceptfds, timeout, NULL);
    if (TestControl.verbos) {
        klog("%d: select(%d, %x, %x, %x, %x) = %d\n", CURRENT_TASK()->psid,
             nfds, readfds, writefds, exceptfds, timeout, ret);
    }
    return ret;
}

static int sys_newselect(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, const struct timespec *timeout,
                   void *sigmask)
{
    int ret = do_select(nfds, readfds, writefds, exceptfds, timeout, sigmask);
    if (TestControl.verbos) {
        klog("%d: pselect(%d, %x, %x, %x, %x, %x) = %d\n", CURRENT_TASK()->psid,
             nfds, readfds, writefds, exceptfds, timeout, sigmask, ret);
    }
}