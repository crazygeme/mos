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
	sys_lchown,
	0,
	sys_oldstat,
	sys_lseek,
	sys_getpid, // 16 ~ 20
	sys_mount,
	sys_umount,
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
	sys_sync,
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
	sys_setsid,
	0,
	0,
	0,
	sys_setreuid, // 66 ~ 70
	sys_setregid,
	0,
	0,
	sys_sethostname,
	0, // 71 ~ 75
	sys_getrlimit,
	sys_getrusage,
	sys_gettimeofday,
	sys_settimeofday,
	0, // 76 ~ 80
	0,
	sys_select,
	sys_symlink,
	0,
	sys_readlink, // 81 ~ 85
	0,
	0,
	sys_reboot,
	sys_readdir,
	sys_mmap, // 86 ~ 90
	sys_munmap,
	0,
	0,
	sys_fchmod,
	sys_fchown, // 91 ~ 95
	0,
	0,
	0,
	0,
	0, // 96 ~ 100
	0,
	sys_socketcall,
	sys_syslog,
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
	sys_sigreturn,
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
	sys_poll,
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
	sys_chown,
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
	0,
	0,
	0, // 196 ~ 200
	0,
	0,
	0,
	0,
	0, // 201 ~ 205
	0,
	0,
	0,
	0,
	0, // 206 ~ 210
	0,
	0,
	0,
	0,
	0, // 211 ~ 215
	0,
	0,
	0,
	0,
	0, // 216 ~ 220
	sys_fcntl64,
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
