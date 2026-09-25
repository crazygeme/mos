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
 *   ps/ps_signal.c  — signal delivery and signal-related syscalls
 *   elf/exec.c      — sys_execve
 *   fs/syslog.c     — sys_syslog
 */

#include <int/int.h>
#include <ps/ps.h>
#include <elf/exec.h>
#include <lib/klib.h>
#include <ps/signal.h>
#include <hw/time.h>
#include <config.h>
#include <errno.h>
#include <macro.h>
#include "syscall_internal.h"

/* handlers defined in other subsystems */
int sys_getrusage(int who, rusage *usage);
int sys_syslog(int type, char *buf, int len);
typedef int (*syscall_fn)(unsigned ebx, unsigned ecx, unsigned edx,
			  unsigned esi, unsigned edi, unsigned ebp);

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2)
{
	printk("test call: arg0 %x, arg1 %x, arg2 %x\n", arg0, arg1, arg2);
	return 0;
}

static unsigned call_table[NR_syscalls] = {
	[0] = test_call, // 0   __NR_restart_syscall
	[1] = sys_exit, // 1   __NR_exit
	[2] = sys_fork, // 2   __NR_fork
	[3] = sys_read, // 3   __NR_read
	[4] = sys_write, // 4   __NR_write
	[5] = sys_open, // 5   __NR_open
	[6] = sys_close, // 6   __NR_close
	[7] = sys_waitpid, // 7   __NR_waitpid
	[8] = sys_creat, // 8   __NR_creat
	[9] = sys_link, // 9   __NR_link
	[10] = sys_unlink, // 10  __NR_unlink
	[11] = sys_execve, // 11  __NR_execve
	[12] = sys_chdir, // 12  __NR_chdir
	[13] = sys_time, // 13  __NR_time
	[14] = sys_mknod, // 14  __NR_mknod
	[15] = sys_chmod, // 15  __NR_chmod
	[16] = sys_lchown, // 16  __NR_lchown
	[17] = sys_break, // 17  __NR_break
	[18] = sys_oldstat, // 18  __NR_oldstat
	[19] = sys_lseek, // 19  __NR_lseek
	[20] = sys_getpid, // 20  __NR_getpid
	[21] = sys_mount, // 21  __NR_mount
	[22] = sys_umount, // 22  __NR_umount
	[23] = sys_setuid, // 23  __NR_setuid
	[24] = sys_getuid, // 24  __NR_getuid
	[25] = sys_stime, // 25  __NR_stime
	[26] = sys_ptrace, // 26  __NR_ptrace
	[27] = sys_alarm, // 27  __NR_alarm
	[28] = 0, // __NR_oldfstat
	[29] = sys_pause, // 29  __NR_pause
	[30] = sys_utime, // 30  __NR_utime
	[31] = sys_stty, // 31  __NR_stty
	[32] = sys_gtty, // 32  __NR_gtty
	[33] = sys_access, // 33  __NR_access
	[34] = sys_nice, // 34  __NR_nice
	[35] = sys_ftime, // 35  __NR_ftime
	[36] = sys_sync, // 36  __NR_sync
	[37] = sys_kill, // 37  __NR_kill
	[38] = sys_rename, // 38  __NR_rename
	[39] = sys_mkdir, // 39  __NR_mkdir
	[40] = sys_rmdir, // 40  __NR_rmdir
	[41] = sys_dup, // 41  __NR_dup
	[42] = sys_pipe, // 42  __NR_pipe
	[43] = sys_times, // 43  __NR_times
	[44] = sys_prof, // 44  __NR_prof
	[45] = sys_brk, // 45  __NR_brk
	[46] = sys_setgid, // 46  __NR_setgid
	[47] = sys_getgid, // 47  __NR_getgid
	[48] = sys_signal, // 48  __NR_signal
	[49] = sys_geteuid, // 49  __NR_geteuid
	[50] = sys_getegid, // 50  __NR_getegid
	[51] = sys_acct, // 51  __NR_acct
	[52] = sys_umount2, // 52  __NR_umount2
	[53] = sys_lock, // 53  __NR_lock
	[54] = sys_ioctl, // 54  __NR_ioctl
	[55] = sys_fcntl, // 55  __NR_fcntl
	[56] = 0, // __NR_mpx
	[57] = sys_setpgid, // 57  __NR_setpgid
	[58] = 0, // __NR_ulimit
	[59] = 0, // __NR_oldolduname
	[60] = sys_umask, // 60  __NR_umask
	[61] = sys_chroot, // 61  __NR_chroot
	[62] = 0, // __NR_ustat
	[63] = sys_dup2, // 63  __NR_dup2
	[64] = sys_getppid, // 64  __NR_getppid
	[65] = sys_getpgrp, // 65  __NR_getpgrp
	[66] = sys_setsid, // 66  __NR_setsid
	[67] = sys_sigaction, // 67  __NR_sigaction
	[68] = 0, // __NR_sgetmask
	[69] = 0, // __NR_ssetmask
	[70] = sys_setreuid, // 70  __NR_setreuid
	[71] = sys_setregid, // 71  __NR_setregid
	[72] = 0, // __NR_sigsuspend
	[73] = 0, // __NR_sigpending
	[74] = sys_sethostname, // 74  __NR_sethostname
	[75] = sys_setrlimit, // 75  __NR_setrlimit
	[76] = sys_getrlimit, // 76  __NR_getrlimit
	[77] = sys_getrusage, // 77  __NR_getrusage
	[78] = sys_gettimeofday, // 78  __NR_gettimeofday
	[79] = sys_settimeofday, // 79  __NR_settimeofday
	[80] = sys_getgroups, // 80  __NR_getgroups
	[81] = sys_setgroups, // 81  __NR_setgroups
	[82] = sys_select, // 82  __NR_select
	[83] = sys_symlink, // 83  __NR_symlink
	[84] = 0, // __NR_oldlstat
	[85] = sys_readlink, // 85  __NR_readlink
	[86] = 0, // __NR_uselib
	[87] = 0, // __NR_swapon
	[88] = sys_reboot, // 88  __NR_reboot
	[89] = sys_readdir, // 89  __NR_readdir
	[90] = sys_mmap, // 90  __NR_mmap
	[91] = sys_munmap, // 91  __NR_munmap
	[92] = 0, // __NR_truncate
	[93] = sys_ftruncate, // 93  __NR_ftruncate
	[94] = sys_fchmod, // 94  __NR_fchmod
	[95] = sys_fchown, // 95  __NR_fchown
	[96] = sys_getpriority, // 96  __NR_getpriority
	[97] = sys_setpriority, // 97  __NR_setpriority
	[98] = 0, // __NR_profil
	[99] = sys_statfs, // 99  __NR_statfs
	[100] = sys_fstatfs, // 100 __NR_fstatfs
	[101] = sys_ioperm, // 101 __NR_ioperm
	[102] = sys_socketcall, // 102 __NR_socketcall
	[103] = sys_syslog, // 103 __NR_syslog
	[104] = sys_setitimer, // 104 __NR_setitimer
	[105] = sys_getitimer, // 105 __NR_getitimer
	[106] = sys_stat, // 106 __NR_stat
	[107] = sys_lstat, // 107 __NR_lstat
	[108] = sys_fstat, // 108 __NR_fstat
	[109] = 0, // __NR_olduname
	[110] = sys_iopl, // 110 __NR_iopl
	[111] = sys_vhangup, // 111 __NR_vhangup
	[112] = 0, // __NR_idle
	[113] = sys_vm86old, // 113 __NR_vm86old
	[114] = sys_wait4, // 114 __NR_wait4
	[115] = 0, // __NR_swapoff
	[116] = sys_sysinfo, // 116 __NR_sysinfo
	[117] = sys_ipc, // 117 __NR_ipc
	[118] = sys_fsync, // 118 __NR_fsync
	[119] = sys_sigreturn, // 119 __NR_sigreturn
	[120] = sys_clone, // 120 __NR_clone
	[121] = 0, // __NR_setdomainname
	[122] = sys_uname, // 122 __NR_uname
	[123] = sys_modify_ldt, // 123 __NR_modify_ldt
	[124] = 0, // __NR_adjtimex
	[125] = sys_mprotect, // 125 __NR_mprotect
	[126] = 0, // __NR_sigprocmask
	[127] = 0, // __NR_create_module
	[128] = 0, // __NR_init_module
	[129] = 0, // __NR_delete_module
	[130] = 0, // __NR_get_kernel_syms
	[131] = sys_quotactl, // 131 __NR_quotactl
	[132] = sys_getpgid, // 132 __NR_getpgid
	[133] = sys_fchdir, // 133 __NR_fchdir
	[134] = 0, // __NR_bdflush
	[135] = 0, // __NR_sysfs
	[136] = sys_personality, // 136 __NR_personality
	[137] = 0, // __NR_afs_syscall
	[138] = sys_setfsuid, // 138 __NR_setfsuid
	[139] = sys_setfsgid, // 139 __NR_setfsgid
	[140] = sys_llseek, // 140 __NR__llseek
	[141] = sys_getdents, // 141 __NR_getdents
	[142] = sys_newselect, // 142 __NR__newselect
	[143] = sys_flock, // 143 __NR_flock
	[144] = 0, // __NR_msync
	[145] = sys_readv, // 145 __NR_readv
	[146] = sys_writev, // 146 __NR_writev
	[147] = sys_getsid, // 147 __NR_getsid
	[148] = 0, // __NR_fdatasync
	[149] = sys__sysctl, // 149 __NR__sysctl
	[150] = sys_mlock, // 150 __NR_mlock
	[151] = sys_munlock, // 151 __NR_munlock
	[152] = sys_mlockall, // 152 __NR_mlockall
	[153] = sys_munlockall, // 153 __NR_munlockall
	[154] = sys_sched_setparam, // 154 __NR_sched_setparam
	[155] = sys_sched_getparam, // 155 __NR_sched_getparam
	[156] = sys_sched_setscheduler, // 156 __NR_sched_setscheduler
	[157] = sys_sched_getscheduler, // 157 __NR_sched_getscheduler
	[158] = sys_sched_yield, // 158 __NR_sched_yield
	[159] = sys_sched_get_priority_max, // 159 __NR_sched_get_priority_max
	[160] = sys_sched_get_priority_min, // 160 __NR_sched_get_priority_min
	[161] = sys_sched_rr_get_interval, // 161 __NR_sched_rr_get_interval
	[162] = sys_nanosleep, // 162 __NR_nanosleep
	[163] = sys_mremap, // 163 __NR_mremap
	[164] = sys_setresuid, // 164 __NR_setresuid
	[165] = sys_getresuid, // 165 __NR_getresuid
	[166] = sys_vm86, // 166 __NR_vm86
	[167] = sys_query_module, // 167 __NR_query_module
	[168] = sys_poll, // 168 __NR_poll
	[169] = 0, // __NR_nfsservctl
	[170] = sys_setresgid, // 170 __NR_setresgid
	[171] = sys_getresgid, // 171 __NR_getresgid
	[172] = sys_prctl, // __NR_prctl
	[173] = sys_rt_sigreturn, // 173 __NR_rt_sigreturn
	[174] = sys_rt_sigaction, // 174 __NR_rt_sigaction
	[175] = sys_rt_sigprocmask, // 175 __NR_rt_sigprocmask
	[176] = sys_rt_sigpending, // 176 __NR_rt_sigpending
	[177] = sys_rt_sigtimedwait, // 177 __NR_rt_sigtimedwait
	[178] = sys_rt_sigqueueinfo, // 178 __NR_rt_sigqueueinfo
	[179] = sys_rt_sigsuspend, // 179 __NR_rt_sigsuspend
	[180] = sys_pread64, // 180 __NR_pread64
	[181] = sys_pwrite64, // 181 __NR_pwrite64
	[182] = sys_chown, // 182 __NR_chown
	[183] = sys_getcwd, // 183 __NR_getcwd
	[184] = sys_capget, // __NR_capget
	[185] = sys_capset, // __NR_capset
	[186] = sys_sigaltstack, // 186 __NR_sigaltstack
	[187] = 0, // __NR_sendfile
	[188] = 0, // __NR_getpmsg
	[189] = 0, // __NR_putpmsg
	[190] = sys_vfork, // 190 __NR_vfork
	[191] = sys_ugetrlimit, // 191 __NR_ugetrlimit
	[192] = sys_mmap2, // 192 __NR_mmap2
	[193] = 0, // __NR_truncate64
	[194] = sys_ftruncate64, // 194 __NR_ftruncate64
	[195] = sys_stat64, // 195 __NR_stat64
	[196] = sys_lstat64, // 196 __NR_lstat64
	[197] = sys_fstat64, // 197 __NR_fstat64
	[198] = 0, // __NR_lchown32
	[199] = sys_getuid32, // 199 __NR_getuid32
	[200] = sys_getgid32, // 200 __NR_getgid32
	[201] = sys_geteuid32, // 201 __NR_geteuid32
	[202] = sys_getegid32, // 202 __NR_getegid32
	[203] = sys_setreuid32, // 203 __NR_setreuid32
	[204] = sys_setregid32, // 204 __NR_setregid32
	[205] = sys_getgroups32, // 205 __NR_getgroups32
	[206] = sys_setgroups32, // 206 __NR_setgroups32
	[207] = sys_fchmod, // 207 __NR_fchown32
	[208] = sys_setresuid32, // 208 __NR_setresuid32
	[209] = sys_getresuid32, // 209 __NR_getresuid32
	[210] = sys_setresgid32, // 210 __NR_setresgid32
	[211] = sys_getresgid32, // 211 __NR_getresgid32
	[212] = sys_chown, // 212 __NR_chown32
	[213] = sys_setuid32, // 213 __NR_setuid32
	[214] = sys_setgid32, // 214 __NR_setgid32
	[215] = sys_setfsuid32, // 215 __NR_setfsuid32
	[216] = sys_setfsgid32, // 216 __NR_setfsgid32
	[217] = 0, // __NR_pivot_root
	[218] = 0, // __NR_mincore
	[219] = sys_madvise,
	[220] = sys_getdents64, // 220 __NR_getdents64
	[221] = sys_fcntl64, // 221 __NR_fcntl64
	[222] = 0,
	[223] = 0,
	[224] = sys_gettid, // 224 __NR_gettid
	[225] = sys_readahead, // 225 __NR_readahead
	[226] = sys_setxattr, // 226 __NR_setxattr
	[227] = sys_lsetxattr, // 227 __NR_lsetxattr
	[228] = sys_fsetxattr, // 228 __NR_fsetxattr
	[229] = sys_getxattr, // 229 __NR_getxattr
	[230] = sys_lgetxattr, // 230 __NR_lgetxattr
	[231] = sys_fgetxattr, // 231 __NR_fgetxattr
	[232] = sys_listxattr, // 232 __NR_listxattr
	[233] = sys_llistxattr, // 233 __NR_llistxattr
	[234] = sys_flistxattr, // 234 __NR_flistxattr
	[235] = sys_removexattr, // 235 __NR_removexattr
	[236] = sys_lremovexattr, // 236 __NR_lremovexattr
	[237] = sys_fremovexattr, // 237 __NR_fremovexattr
	[238] = sys_tkill, // 238 __NR_tkill
	[239] = 0, // __NR_sendfile64
	[240] = sys_futex, // 240 __NR_futex
	[241] = 0, // __NR_sched_setaffinity
	[242] = 0, // __NR_sched_getaffinity
	[243] = sys_set_thread_area, // 243 __NR_set_thread_area
	[244] = sys_get_thread_area, // 244 __NR_get_thread_area
	[245] = 0, // __NR_io_setup
	[246] = 0, // __NR_io_destroy
	[247] = 0, // __NR_io_getevents
	[248] = 0, // __NR_io_submit
	[249] = 0, // __NR_io_cancel
	[250] = 0, // __NR_fadvise64
	[251] = 0,
	[252] = sys_exit_group, // 252 __NR_exit_group
	[253] = 0, // __NR_lookup_dcookie
	[254] = 0, // __NR_epoll_create
	[255] = 0, // __NR_epoll_ctl
	[256] = 0, // __NR_epoll_wait
	[257] = 0, // __NR_remap_file_pages
	[258] = sys_set_tid_address, // 258 __NR_set_tid_address
	[259] = sys_timer_create,
	[260] = sys_timer_settime,
	[261] = sys_timer_gettime,
	[262] = sys_timer_getoverrun,
	[263] = sys_timer_delete,
	[267] = sys_clock_nanosleep, // 267 __NR_clock_nanosleep
	[265] = sys_clock_gettime,
	[295] = sys_openat,
	[297] = sys_mknodat,
	[300] = sys_fstatat64,
	[306] = sys_fchmodat, // 306 __NR_fchmodat
	[307] = sys_faccessat,
	[308] = sys_pselect6,
	[309] = sys_ppoll,
	[331] = sys_pipe2, // 331 __NR_pipe2
	[332] = sys_inotify_init1, // 332 __NR_inotify_init1
	[340] = sys_prlimit64,
	[337] = sys_recvmmsg, // 337 __NR_recvmmsg
	[345] = sys_sendmmsg, // 345 __NR_sendmmsg
	[292] = sys_inotify_add_watch, // 292 __NR_inotify_add_watch
	[293] = sys_inotify_rm_watch, // 293 __NR_inotify_rm_watch
	[311] = sys_set_robust_list,
	[312] = sys_get_robust_list,
	[355] = sys_getrandom,
	[383] = sys_statx,
	[386] = sys_rseq,
	[403] = sys_clock_gettime64,
	[436] = sys_close_range,
	[439] = sys_faccessat2,
};

static int unhandled_syscall(unsigned callno)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("unhandled syscall %d\n", callno);
	return -ENOSYS;
}

static void syscall_process(intr_frame *frame)
{
	syscall_fn fn;
	int ret;

	ps_ptrace_maybe_stop_syscall(frame, 1);

	if (frame->eax >= NR_syscalls) {
		frame->eax = unhandled_syscall(frame->eax);
		ps_ptrace_maybe_stop_syscall(frame, 0);
		return;
	}

	fn = (syscall_fn)call_table[frame->eax];
	if (!fn)
		ret = unhandled_syscall(frame->eax);
	else
		ret = fn(frame->ebx, frame->ecx, frame->edx, frame->esi,
			 frame->edi, frame->ebp);

	frame->eax = ret;
	ps_ptrace_maybe_stop_syscall(frame, 0);
}

static void syscall_init()
{
	int_register(SYSCALL_INT_NO, syscall_process, 0, 3);
}

KERNEL_INIT(7, syscall_init);
