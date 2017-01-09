#ifndef _UNISTD_H_
#define _UNISTD_H_

#define __NR_restart_syscall 0
#define __NR_exit 1
#define __NR_fork 2
#define __NR_read 3
#define __NR_write 4
#define __NR_open 5
#define __NR_close 6
#define __NR_waitpid 7
#define __NR_creat 8
#define __NR_link 9
#define __NR_unlink 10
#define __NR_execve 11
#define __NR_chdir 12
#define __NR_time 13
#define __NR_mknod 14
#define __NR_chmod 15
#define __NR_lchown 16
#define __NR_break 17
#define __NR_oldstat 18
#define __NR_lseek 19
#define __NR_getpid 20
#define __NR_mount 21
#define __NR_umount 22
#define __NR_setuid 23
#define __NR_getuid 24
#define __NR_stime 25
#define __NR_ptrace 26
#define __NR_alarm 27
#define __NR_oldfstat 28
#define __NR_pause 29
#define __NR_utime 30
#define __NR_stty 31
#define __NR_gtty 32
#define __NR_access 33
#define __NR_nice 34
#define __NR_ftime 35
#define __NR_sync 36
#define __NR_kill 37
#define __NR_rename 38
#define __NR_mkdir 39
#define __NR_rmdir 40
#define __NR_dup 41
#define __NR_pipe 42
#define __NR_times 43
#define __NR_prof 44
#define __NR_brk 45
#define __NR_setgid 46
#define __NR_getgid 47
#define __NR_signal 48
#define __NR_geteuid 49
#define __NR_getegid 50
#define __NR_acct 51
#define __NR_umount2 52
#define __NR_lock 53
#define __NR_ioctl 54
#define __NR_fcntl 55
#define __NR_mpx 56
#define __NR_setpgid 57
#define __NR_ulimit 58
#define __NR_oldolduname 59
#define __NR_umask 60
#define __NR_chroot 61
#define __NR_ustat 62
#define __NR_dup2 63
#define __NR_getppid 64
#define __NR_getpgrp 65
#define __NR_setsid 66
#define __NR_sigaction 67
#define __NR_sgetmask 68
#define __NR_ssetmask 69
#define __NR_setreuid 70
#define __NR_setregid 71
#define __NR_sigsuspend 72
#define __NR_sigpending 73
#define __NR_sethostname 74
#define __NR_setrlimit 75
#define __NR_getrlimit 76 /* Back compatible 2Gig limited rlimit */
#define __NR_getrusage 77
#define __NR_gettimeofday 78
#define __NR_settimeofday 79
#define __NR_getgroups 80
#define __NR_setgroups 81
#define __NR_select 82
#define __NR_symlink 83
#define __NR_oldlstat 84
#define __NR_readlink 85
#define __NR_uselib 86
#define __NR_swapon 87
#define __NR_reboot 88
#define __NR_readdir 89
#define __NR_mmap 90
#define __NR_munmap 91
#define __NR_truncate 92
#define __NR_ftruncate 93
#define __NR_fchmod 94
#define __NR_fchown 95
#define __NR_getpriority 96
#define __NR_setpriority 97
#define __NR_profil 98
#define __NR_statfs 99
#define __NR_fstatfs 100
#define __NR_ioperm 101
#define __NR_socketcall 102
#define __NR_syslog 103
#define __NR_setitimer 104
#define __NR_getitimer 105
#define __NR_stat 106
#define __NR_lstat 107
#define __NR_fstat 108
#define __NR_olduname 109
#define __NR_iopl 110
#define __NR_vhangup 111
#define __NR_idle 112
#define __NR_vm86old 113
#define __NR_wait4 114
#define __NR_swapoff 115
#define __NR_sysinfo 116
#define __NR_ipc 117
#define __NR_fsync 118
#define __NR_sigreturn 119
#define __NR_clone 120
#define __NR_setdomainname 121
#define __NR_uname 122
#define __NR_modify_ldt 123
#define __NR_adjtimex 124
#define __NR_mprotect 125
#define __NR_sigprocmask 126
#define __NR_create_module 127
#define __NR_init_module 128
#define __NR_delete_module 129
#define __NR_get_kernel_syms 130
#define __NR_quotactl 131
#define __NR_getpgid 132
#define __NR_fchdir 133
#define __NR_bdflush 134
#define __NR_sysfs 135
#define __NR_personality 136
#define __NR_afs_syscall 137 /* Syscall for Andrew File System */
#define __NR_setfsuid 138
#define __NR_setfsgid 139
#define __NR__llseek 140
#define __NR_getdents 141
#define __NR__newselect 142
#define __NR_flock 143
#define __NR_msync 144
#define __NR_readv 145
#define __NR_writev 146
#define __NR_getsid 147
#define __NR_fdatasync 148
#define __NR__sysctl 149
#define __NR_mlock 150
#define __NR_munlock 151
#define __NR_mlockall 152
#define __NR_munlockall 153
#define __NR_sched_setparam 154
#define __NR_sched_getparam 155
#define __NR_sched_setscheduler 156
#define __NR_sched_getscheduler 157
#define __NR_sched_yield 158
#define __NR_sched_get_priority_max 159
#define __NR_sched_get_priority_min 160
#define __NR_sched_rr_get_interval 161
#define __NR_nanosleep 162
#define __NR_mremap 163
#define __NR_setresuid 164
#define __NR_getresuid 165
#define __NR_vm86 166
#define __NR_query_module 167
#define __NR_poll 168
#define __NR_nfsservctl 169
#define __NR_setresgid 170
#define __NR_getresgid 171
#define __NR_prctl 172
#define __NR_rt_sigreturn 173
#define __NR_rt_sigaction 174
#define __NR_rt_sigprocmask 175
#define __NR_rt_sigpending 176
#define __NR_rt_sigtimedwait 177
#define __NR_rt_sigqueueinfo 178
#define __NR_rt_sigsuspend 179
#define __NR_pread64 180
#define __NR_pwrite64 181
#define __NR_chown 182
#define __NR_getcwd 183
#define __NR_capget 184
#define __NR_capset 185
#define __NR_sigaltstack 186
#define __NR_sendfile 187
#define __NR_getpmsg 188 /* some people actually want streams */
#define __NR_putpmsg 189 /* some people actually want streams */
#define __NR_vfork 190
#define __NR_ugetrlimit 191 /* SuS compliant getrlimit */
#define __NR_mmap2 192
#define __NR_lstat64 196
#define __NR_fstat64 197

#define __NR_quota 198
#define NR_syscalls 199

#define	_SYS_NAMELEN	65 // in linux 2.1


/// TTY
#define IOCTL_TTY 0
#define IOCTL_MAX 1

/// reboot
   /*
	* Commands accepted by the _reboot() system call.                                                                                                       
	*
	* RESTART     Restart system using default command and mode.
	* HALT        Stop OS and give system control to ROM monitor, if any.
	* CAD_ON      Ctrl-Alt-Del sequence causes RESTART command.
	* CAD_OFF     Ctrl-Alt-Del sequence sends SIGINT to init task.
	* POWER_OFF   Stop OS and remove all power from system, if possible.
	* RESTART2    Restart system using given command string.
	* SW_SUSPEND  Suspend system using software suspend if compiled in.
	* KEXEC       Restart system using a previously loaded Linux kernel                                                                                     
	*/
 
#define MOS_REBOOT_CMD_RESTART    0x01234567
#define MOS_REBOOT_CMD_HALT       0xCDEF0123
#define MOS_REBOOT_CMD_CAD_ON     0x89ABCDEF
#define MOS_REBOOT_CMD_CAD_OFF    0x00000000
#define MOS_REBOOT_CMD_POWER_OFF  0x4321FEDC
#define MOS_REBOOT_CMD_RESTART2   0xA1B2C3D4
#define MOS_REBOOT_CMD_SW_SUSPEND 0xD000FCE2
#define MOS_REBOOT_CMD_KEXEC      0x45584543   

struct	utsname {
	char	sysname[_SYS_NAMELEN];	/* [XSI] Name of OS */
	char	nodename[_SYS_NAMELEN];	/* [XSI] Name of this network node */
	char	release[_SYS_NAMELEN];	/* [XSI] Release level */
	char	version[_SYS_NAMELEN];	/* [XSI] Version level */
	char	machine[_SYS_NAMELEN];	/* [XSI] Hardware type */
	char	domain[_SYS_NAMELEN];
};

// oldstat
struct oldstat  
{  
	unsigned short st_dev;
    unsigned short st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned short st_rdev;
    unsigned long st_size;
    unsigned long st_atime;
    unsigned long st_mtime;
    unsigned long st_ctime;
  
};

struct stat {
    unsigned st_dev; /* ID of device containing file*/
    void* st_ino; /* inode number */
    unsigned st_mode; /* protection mode*/
    unsigned st_nlink; /* number of hard links */
    unsigned short st_uid; /* user ID of owner -user id*/
    unsigned short st_gid; /* group ID of owner - group id*/
    unsigned st_rdev; /* device ID (if special file)*/
    unsigned st_size; /* total size, in bytes*/
    unsigned st_blksize; /* blocksize for filesystem I/O*/
    unsigned st_blocks; /* number of blocks allocated*/
    unsigned st_atime; /* time of last access*/
    unsigned st_mtime; /* time of last modification*/
    unsigned st_ctime; /* time of last status change */ 
};


 
struct stat64 {
    unsigned long long st_dev; 	//0
    unsigned __pad1;		//8
    unsigned st_ino;		//12
    unsigned st_mode;		//16
    unsigned st_nlink;		//20
    unsigned st_uid;		//24
    unsigned st_gid;		//28
    unsigned long long st_rdev;	//32
    unsigned __pad2;		//40
    unsigned long long st_size;		//44
    unsigned st_blksize;	//48
    unsigned long long st_blocks;		//52
    unsigned st_atime;		//56
    unsigned __pad3;		//60
    unsigned st_mtime;		//64
    unsigned __pad4;		//68
    unsigned st_ctime;		//72
    unsigned __pad5;
    unsigned __pad6;
    unsigned __pad7;
    unsigned __pad8;
};

struct krnquota
{
	unsigned heap_cur;  // current usage
	unsigned heap_wm; 	// water mask
	unsigned heap_top;
	unsigned phymm_max;
	unsigned phymm_cur;
	unsigned phymm_wm;
	unsigned pgc_count;
	unsigned pgc_top;
	unsigned sched_spent;
	unsigned total_spent;
    unsigned page_fault;
};

#define R_OK 4 // test read access right
#define W_OK 2 // test write access right
#define X_OK 1 // test execution access right
#define F_OK 0 // test file exist

#endif
