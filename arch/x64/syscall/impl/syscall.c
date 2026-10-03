/* AMD64 and IA-32 syscall namespaces remain separate. */
#include <ps/ps.h>
#include <ps/task.h>
#include <int/int.h>
#include <mm/mm.h>
#include <elf/exec.h>
#include <macro.h>
#include <errno.h>
#include <syscall/impl/syscall_internal.h>
extern void i386_syscall_process(intr_frame *);
extern int native_sigaction(int, const void *, void *, unsigned);
extern intptr_t native_sigreturn(intr_frame *);
extern int native_sigaltstack(const void *, void *);
extern int native_stat(unsigned, uintptr_t, void *, int);
extern int native_clock_gettime(int, void *);
extern int native_gettimeofday(void *, struct timezone *);
extern int native_nanosleep(const void *, void *);
extern int native_futex(int *, int, int, const void *, int *, int);
extern int native_rlimit(int, void *, int);
extern int native_wait4(int, int *, int, void *);
extern int native_getrusage(int, void *);
extern int native_fcntl(int, int, uintptr_t);
extern int sys_getrusage(int, rusage *);
extern int sys_syslog(int, char *, int);

typedef int (*native_fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
			 uintptr_t);
static uintptr_t native_calls[440] = {
	[0] = sys_read,
	[1] = sys_write,
	[2] = sys_open,
	[3] = sys_close,
	[7] = sys_poll,
	[10] = sys_mprotect,
	[11] = sys_munmap,
	[14] = sys_rt_sigprocmask,
	[16] = sys_ioctl,
	[19] = sys_readv,
	[20] = sys_writev,
	[21] = sys_access,
	[22] = sys_pipe,
	[24] = sys_sched_yield,
	[28] = sys_madvise,
	[32] = sys_dup,
	[33] = sys_dup2,
	[34] = sys_pause,
	[37] = sys_alarm,
	[39] = sys_getpid,
	[46] = sys_sendmsg,
	[47] = sys_recvmsg,
	[57] = sys_fork,
	[58] = sys_vfork,
	[59] = sys_execve,
	[60] = sys_exit,
	[62] = sys_kill,
	[63] = sys_uname,
	[73] = sys_flock,
	[74] = sys_fsync,
	[79] = sys_getcwd,
	[80] = sys_chdir,
	[81] = sys_fchdir,
	[82] = sys_rename,
	[83] = sys_mkdir,
	[84] = sys_rmdir,
	[85] = sys_creat,
	[86] = sys_link,
	[87] = sys_unlink,
	[88] = sys_symlink,
	[89] = sys_readlink,
	[90] = sys_chmod,
	[91] = sys_fchmod,
	[92] = sys_chown,
	[93] = sys_fchown,
	[94] = sys_lchown,
	[95] = sys_umask,
	[102] = sys_getuid32,
	[103] = sys_syslog,
	[104] = sys_getgid32,
	[105] = sys_setuid32,
	[106] = sys_setgid32,
	[107] = sys_geteuid32,
	[108] = sys_getegid32,
	[109] = sys_setpgid,
	[110] = sys_getppid,
	[111] = sys_getpgrp,
	[112] = sys_setsid,
	[113] = sys_setreuid32,
	[114] = sys_setregid32,
	[115] = sys_getgroups32,
	[116] = sys_setgroups32,
	[117] = sys_setresuid32,
	[118] = sys_getresuid32,
	[119] = sys_setresgid32,
	[120] = sys_getresgid32,
	[121] = sys_getpgid,
	[122] = sys_setfsuid32,
	[123] = sys_setfsgid32,
	[124] = sys_getsid,
	[125] = sys_capget,
	[126] = sys_capset,
	[127] = sys_rt_sigpending,
	[130] = sys_rt_sigsuspend,
	[133] = sys_mknod,
	[135] = sys_personality,
	[140] = sys_getpriority,
	[141] = sys_setpriority,
	[154] = sys_modify_ldt,
	[161] = sys_chroot,
	[162] = sys_sync,
	[165] = sys_mount,
	[166] = sys_umount2,
	[169] = sys_reboot,
	[170] = sys_sethostname,
	[172] = sys_iopl,
	[173] = sys_ioperm,
	[186] = sys_gettid,
	[200] = sys_tkill,
	[217] = sys_getdents64,
	[218] = sys_set_tid_address,
	[231] = sys_exit_group,
	[254] = sys_inotify_add_watch,
	[255] = sys_inotify_rm_watch,
	[257] = sys_openat,
	[258] = sys_mkdirat,
	[259] = sys_mknodat,
	[263] = sys_unlinkat,
	[264] = sys_renameat,
	[266] = sys_symlinkat,
	[268] = sys_fchmodat,
	[269] = sys_faccessat,
	[273] = sys_set_robust_list,
	[274] = sys_get_robust_list,
	[292] = sys_dup3,
	[293] = sys_pipe2,
	[294] = sys_inotify_init1,
	[302] = sys_prlimit64,
	[318] = sys_getrandom,
	[436] = sys_close_range,
	[439] = sys_faccessat2,
};
static intptr_t arch_prctl(unsigned op, uintptr_t address)
{
	task_struct *task = current;
	switch (op) {
	case 0x1001:
	case 0x1002:
		if (address >= MOS_NATIVE_TASK_SIZE)
			return -EPERM;
		if (op == 0x1002)
			task->tss.fs_base = address;
		else
			task->tss.gs_base = address;
		ps_load_task_segments(task);
		return 0;
	case 0x1003:
	case 0x1004:
		if (!address ||
		    address > MOS_NATIVE_TASK_SIZE - sizeof(uint64_t))
			return -EFAULT;
		*(uint64_t *)address = op == 0x1003 ? task->tss.fs_base :
						      task->tss.gs_base;
		return 0;
	default:
		return -EINVAL;
	}
}
static intptr_t native_process(intr_frame *f)
{
	uintptr_t a = f->edi, b = f->esi, c = f->edx, d = f->r10, e = f->r8,
		  g = f->r9;
	switch (f->eax) {
	case 4:
	case 5:
	case 6:
		return native_stat(f->eax, a, (void *)b, 0);
	case 8: {
		uint64_t result;
		int ret = sys_llseek(a, b >> 32, b, &result, c);
		return ret < 0 ? ret : (intptr_t)result;
	}
	case 9:
		if (g > 0x7fffffff || (g & (PAGE_SIZE - 1)))
			return -EINVAL;
		return do_mmap(a, b, c, d, e, g);
	case 12:
		return sys_brk(a);
	case 13:
		return native_sigaction(a, (void *)b, (void *)c, d);
	case 15:
		return native_sigreturn(f);
	case 17:
		return sys_pread64(a, (void *)b, c, d, d >> 32);
	case 18:
		return sys_pwrite64(a, (void *)b, c, d, d >> 32);
	case 35:
		return native_nanosleep((void *)a, (void *)b);
	case 56:
		return sys_clone(a, b, (void *)c, (void *)e, (void *)d);
	case 61:
		return native_wait4(a, (void *)b, c, (void *)d);
	case 72:
		return native_fcntl(a, b, c);
	case 96:
		return native_gettimeofday((void *)a, (void *)b);
	case 97:
		return native_rlimit(a, (void *)b, 0);
	case 98:
		return native_getrusage(a, (void *)b);
	case 131:
		return native_sigaltstack((void *)a, (void *)b);
	case 158:
		return arch_prctl(a, b);
	case 160:
		return native_rlimit(a, (void *)b, 1);
	case 201: {
		unsigned seconds;
		int ret = sys_time(&seconds);
		if (ret < 0)
			return ret;
		if (a)
			*(uint64_t *)a = seconds;
		return seconds;
	}
	case 202:
		return native_futex((void *)a, b, c, (void *)d, (void *)e, g);
	case 228:
		return native_clock_gettime(a, (void *)b);
	case 262:
		return native_stat(f->eax, b, (void *)c, a);
	default:
		if (f->eax >= sizeof(native_calls) / sizeof(native_calls[0]) ||
		    !native_calls[f->eax])
			return -ENOSYS;
		return ((native_fn)native_calls[f->eax])(a, b, c, d, e, g);
	}
}
static void syscall_process(intr_frame *frame)
{
	if (frame->cs != USER64_CODE_SELECTOR) {
		i386_syscall_process(frame);
		return;
	}
	int traced = ps_ptrace_maybe_stop_syscall(frame, 1);
	frame->eax = native_process(frame);
	if (traced)
		ps_ptrace_maybe_stop_syscall(frame, 0);
}
static void syscall_init(void)
{
	int_register(SYSCALL_INT_NO, syscall_process, 0, 3);
}
KERNEL_INIT(7, syscall_init);
