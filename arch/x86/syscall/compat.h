#ifndef MOS_X86_SYSCALL_COMPAT_H
#define MOS_X86_SYSCALL_COMPAT_H
#define compat_execve sys_execve
#define compat_readv sys_readv
#define compat_writev sys_writev
#define compat_sigaction sys_sigaction
#define compat_sigaltstack sys_sigaltstack
#define compat_ioctl sys_ioctl
#define compat_sendmsg sys_sendmsg
#define compat_recvmsg sys_recvmsg
#define compat_sendmmsg sys_sendmmsg
#define compat_recvmmsg sys_recvmmsg
#endif
