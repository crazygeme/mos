/*
 * syscall.c — syscall dispatch table and interrupt handler.
 *
 * Individual handlers are implemented in:
 *   syscall_io.c   — file descriptor I/O
 *   syscall_fs.c   — filesystem / path operations
 *   syscall_proc.c — process management
 *   syscall_sys.c  — system / miscellaneous
 *
 * Handlers defined outside the syscall layer:
 *   ps/ps_syscall.c — sys_exit, sys_fork, sys_vfork, sys_waitpid, sys_getcwd,
 *                     sys_getrusage
 *   elf/exec.c      — sys_execve
 *   fs/syslog.c     — sys_syslog
 */

#include <int/int.h>
#include <ps/ps.h>
#include <elf/exec.h>
#include <lib/klib.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include "syscall_internal.h"

/* handlers defined in other subsystems */
int sys_getrusage(int who, rusage *usage);
int sys_syslog(int type, char *buf, int len);

/* signal handlers defined in syscall_proc.c */
int sys_sigreturn(void);
void do_signal(intr_frame *frame);

typedef int (*syscall_fn)(unsigned ebx, unsigned ecx, unsigned edx,
			  unsigned esi, unsigned edi, unsigned ebp);

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2)
{
	printk("test call: arg0 %x, arg1 %x, arg2 %x\n", arg0, arg1, arg2);
	return 0;
}

static unsigned call_table[NR_syscalls] = {
	test_call, // 0   __NR_restart_syscall
	sys_exit, // 1   __NR_exit
	sys_fork, // 2   __NR_fork
	sys_read, // 3   __NR_read
	sys_write, // 4   __NR_write
	sys_open, // 5   __NR_open
	sys_close, // 6   __NR_close
	sys_waitpid, // 7   __NR_waitpid
	sys_creat, // 8   __NR_creat
	sys_link, // 9   __NR_link
	sys_unlink, // 10  __NR_unlink
	sys_execve, // 11  __NR_execve
	sys_chdir, // 12  __NR_chdir
	sys_time, // 13  __NR_time
	sys_mknod, // 14  __NR_mknod
	sys_chmod, // 15  __NR_chmod
	sys_lchown, // 16  __NR_lchown
	0, // 17  __NR_break
	sys_oldstat, // 18  __NR_oldstat
	sys_lseek, // 19  __NR_lseek
	sys_getpid, // 20  __NR_getpid
	sys_mount, // 21  __NR_mount
	sys_umount, // 22  __NR_umount
	sys_setuid, // 23  __NR_setuid
	sys_getuid, // 24  __NR_getuid
	0, // 25  __NR_stime
	0, // 26  __NR_ptrace
	sys_alarm, // 27  __NR_alarm
	0, // 28  __NR_oldfstat
	sys_pause, // 29  __NR_pause
	sys_utime, // 30  __NR_utime
	0, // 31  __NR_stty
	0, // 32  __NR_gtty
	sys_access, // 33  __NR_access
	0, // 34  __NR_nice
	0, // 35  __NR_ftime
	sys_sync, // 36  __NR_sync
	sys_kill, // 37  __NR_kill
	sys_rename, // 38  __NR_rename
	sys_mkdir, // 39  __NR_mkdir
	sys_rmdir, // 40  __NR_rmdir
	sys_dup, // 41  __NR_dup
	sys_pipe, // 42  __NR_pipe
	sys_times, // 43  __NR_times
	0, // 44  __NR_prof
	sys_brk, // 45  __NR_brk
	sys_setgid, // 46  __NR_setgid
	sys_getgid, // 47  __NR_getgid
	0, // 48  __NR_signal
	sys_geteuid, // 49  __NR_geteuid
	sys_getegid, // 50  __NR_getegid
	0, // 51  __NR_acct
	0, // 52  __NR_umount2
	0, // 53  __NR_lock
	sys_ioctl, // 54  __NR_ioctl
	sys_fcntl, // 55  __NR_fcntl
	0, // 56  __NR_mpx
	sys_setpgid, // 57  __NR_setpgid
	0, // 58  __NR_ulimit
	0, // 59  __NR_oldolduname
	sys_umask, // 60  __NR_umask
	0, // 61  __NR_chroot
	0, // 62  __NR_ustat
	sys_dup2, // 63  __NR_dup2
	sys_getppid, // 64  __NR_getppid
	sys_getpgrp, // 65  __NR_getpgrp
	sys_setsid, // 66  __NR_setsid
	0, // 67  __NR_sigaction
	0, // 68  __NR_sgetmask
	0, // 69  __NR_ssetmask
	sys_setreuid, // 70  __NR_setreuid
	sys_setregid, // 71  __NR_setregid
	0, // 72  __NR_sigsuspend
	0, // 73  __NR_sigpending
	sys_sethostname, // 74  __NR_sethostname
	sys_setrlimit, // 75  __NR_setrlimit
	sys_getrlimit, // 76  __NR_getrlimit
	sys_getrusage, // 77  __NR_getrusage
	sys_gettimeofday, // 78  __NR_gettimeofday
	sys_settimeofday, // 79  __NR_settimeofday
	sys_getgroups, // 80  __NR_getgroups
	sys_setgroups, // 81  __NR_setgroups
	sys_select, // 82  __NR_select
	sys_symlink, // 83  __NR_symlink
	0, // 84  __NR_oldlstat
	sys_readlink, // 85  __NR_readlink
	0, // 86  __NR_uselib
	0, // 87  __NR_swapon
	sys_reboot, // 88  __NR_reboot
	sys_readdir, // 89  __NR_readdir
	sys_mmap, // 90  __NR_mmap
	sys_munmap, // 91  __NR_munmap
	0, // 92  __NR_truncate
	0, // 93  __NR_ftruncate
	sys_fchmod, // 94  __NR_fchmod
	sys_fchown, // 95  __NR_fchown
	0, // 96  __NR_getpriority
	sys_setpriority, // 97  __NR_setpriority
	0, // 98  __NR_profil
	sys_statfs, // 99  __NR_statfs
	sys_fstatfs, // 100 __NR_fstatfs
	0, // 101 __NR_ioperm
	sys_socketcall, // 102 __NR_socketcall
	sys_syslog, // 103 __NR_syslog
	0, // 104 __NR_setitimer
	0, // 105 __NR_getitimer
	sys_stat, // 106 __NR_stat
	sys_lstat, // 107 __NR_lstat
	sys_fstat, // 108 __NR_fstat
	0, // 109 __NR_olduname
	0, // 110 __NR_iopl
	sys_vhangup, // 111 __NR_vhangup
	0, // 112 __NR_idle
	0, // 113 __NR_vm86old
	sys_wait4, // 114 __NR_wait4
	0, // 115 __NR_swapoff
	sys_sysinfo, // 116 __NR_sysinfo
	0, // 117 __NR_ipc
	sys_fsync, // 118 __NR_fsync
	sys_sigreturn, // 119 __NR_sigreturn
	0, // 120 __NR_clone
	0, // 121 __NR_setdomainname
	sys_uname, // 122 __NR_uname
	0, // 123 __NR_modify_ldt
	0, // 124 __NR_adjtimex
	sys_mprotect, // 125 __NR_mprotect
	0, // 126 __NR_sigprocmask
	0, // 127 __NR_create_module
	0, // 128 __NR_init_module
	0, // 129 __NR_delete_module
	0, // 130 __NR_get_kernel_syms
	0, // 131 __NR_quotactl
	sys_getpgid, // 132 __NR_getpgid
	sys_fchdir, // 133 __NR_fchdir
	0, // 134 __NR_bdflush
	0, // 135 __NR_sysfs
	sys_personality, // 136 __NR_personality
	0, // 137 __NR_afs_syscall
	sys_setfsuid, // 138 __NR_setfsuid
	sys_setfsgid, // 139 __NR_setfsgid
	sys_llseek, // 140 __NR__llseek
	sys_getdents, // 141 __NR_getdents
	sys_newselect, // 142 __NR__newselect
	0, // 143 __NR_flock
	0, // 144 __NR_msync
	sys_readv, // 145 __NR_readv
	sys_writev, // 146 __NR_writev
	sys_getsid, // 147 __NR_getsid
	0, // 148 __NR_fdatasync
	0, // 149 __NR__sysctl
	0, // 150 __NR_mlock
	0, // 151 __NR_munlock
	0, // 152 __NR_mlockall
	0, // 153 __NR_munlockall
	0, // 154 __NR_sched_setparam
	0, // 155 __NR_sched_getparam
	0, // 156 __NR_sched_setscheduler
	0, // 157 __NR_sched_getscheduler
	sys_sched_yield, // 158 __NR_sched_yield
	0, // 159 __NR_sched_get_priority_max
	0, // 160 __NR_sched_get_priority_min
	0, // 161 __NR_sched_rr_get_interval
	sys_nanosleep, // 162 __NR_nanosleep
	0, // 163 __NR_mremap
	sys_setresuid, // 164 __NR_setresuid
	sys_getresuid, // 165 __NR_getresuid
	0, // 166 __NR_vm86
	sys_query_module, // 167 __NR_query_module
	sys_poll, // 168 __NR_poll
	0, // 169 __NR_nfsservctl
	sys_setresgid, // 170 __NR_setresgid
	sys_getresgid, // 171 __NR_getresgid
	0, // 172 __NR_prctl
	0, // 173 __NR_rt_sigreturn
	sys_sigaction, // 174 __NR_rt_sigaction
	sys_sigprocmask, // 175 __NR_rt_sigprocmask
	0, // 176 __NR_rt_sigpending
	0, // 177 __NR_rt_sigtimedwait
	0, // 178 __NR_rt_sigqueueinfo
	0, // 179 __NR_rt_sigsuspend
	sys_pread64, // 180 __NR_pread64
	sys_pwrite64, // 181 __NR_pwrite64
	sys_chown, // 182 __NR_chown
	sys_getcwd, // 183 __NR_getcwd
	0, // 184 __NR_capget
	0, // 185 __NR_capset
	sys_sigaltstack, // 186 __NR_sigaltstack
	0, // 187 __NR_sendfile
	0, // 188 __NR_getpmsg
	0, // 189 __NR_putpmsg
	sys_vfork, // 190 __NR_vfork
	sys_ugetrlimit, // 191 __NR_ugetrlimit
	sys_mmap2, // 192 __NR_mmap2
	0, // 193 __NR_truncate64
	0, // 194 __NR_ftruncate64
	sys_stat64, // 195 __NR_stat64
	sys_lstat64, // 196 __NR_lstat64
	sys_fstat64, // 197 __NR_fstat64
	0, // 198 __NR_lchown32
	sys_getuid32, // 199 __NR_getuid32
	sys_getgid32, // 200 __NR_getgid32
	sys_geteuid32, // 201 __NR_geteuid32
	sys_getegid32, // 202 __NR_getegid32
	sys_setreuid32, // 203 __NR_setreuid32
	sys_setregid32, // 204 __NR_setregid32
	sys_getgroups32, // 205 __NR_getgroups32
	sys_setgroups32, // 206 __NR_setgroups32
	sys_fchmod, // 207 __NR_fchown32
	sys_setresuid32, // 208 __NR_setresuid32
	sys_getresuid32, // 209 __NR_getresuid32
	sys_setresgid32, // 210 __NR_setresgid32
	sys_getresgid32, // 211 __NR_getresgid32
	sys_chown, // 212 __NR_chown32
	sys_setuid32, // 213 __NR_setuid32
	sys_setgid32, // 214 __NR_setgid32
	sys_setfsuid32, // 215 __NR_setfsuid32
	sys_setfsgid32, // 216 __NR_setfsgid32
	0, // 217 __NR_pivot_root
	0, // 218 __NR_mincore
	0, // 219 __NR_madvise
	sys_getdents64, // 220 __NR_getdents64
	sys_fcntl64, // 221 __NR_fcntl64
	0, // 222
	0, // 223
	0, // 224 __NR_gettid
	0, // 225 __NR_readahead
	0, // 226 __NR_setxattr
	0, // 227 __NR_lsetxattr
	0, // 228 __NR_fsetxattr
	0, // 229 __NR_getxattr
	0, // 230 __NR_lgetxattr
	0, // 231 __NR_fgetxattr
	0, // 232 __NR_listxattr
	0, // 233 __NR_llistxattr
	0, // 234 __NR_flistxattr
	0, // 235 __NR_removexattr
	0, // 236 __NR_lremovexattr
	0, // 237 __NR_fremovexattr
	0, // 238 __NR_tkill
	0, // 239 __NR_sendfile64
	0, // 240 __NR_futex
	0, // 241 __NR_sched_setaffinity
	0, // 242 __NR_sched_getaffinity
	0, // 243 __NR_set_thread_area
	0, // 244 __NR_get_thread_area
	0, // 245 __NR_io_setup
	0, // 246 __NR_io_destroy
	0, // 247 __NR_io_getevents
	0, // 248 __NR_io_submit
	0, // 249 __NR_io_cancel
	0, // 250 __NR_fadvise64
	0, // 251
	sys_exit_group, // 252 __NR_exit_group
};

static int unhandled_syscall(unsigned callno)
{
	if (TestControl.verbos)
		klog("unhandled syscall %d\n", callno);
	return -ENOSYS;
}

static void syscall_process(intr_frame *frame)
{
	syscall_fn fn = (syscall_fn)call_table[frame->eax];
	int ret;

	if (!fn)
		ret = unhandled_syscall(frame->eax);
	else
		ret = fn(frame->ebx, frame->ecx, frame->edx, frame->esi,
			 frame->edi, frame->ebp);

	frame->eax = ret;

	/* Deliver any pending signals before returning to user space. */
	do_signal(frame);
}

static void syscall_init()
{
	int_register(SYSCALL_INT_NO, syscall_process, 1, 3);
}

KERNEL_INIT(7, syscall_init);
