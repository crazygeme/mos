#ifndef MOS_I386_COMPAT_H
#define MOS_I386_COMPAT_H
#include <int/int.h>
#if MOS_HAS_NATIVE_USER
int compat_execve(const char *, const uint32_t *, const uint32_t *);
int compat_readv(int, const void *, int);
int compat_writev(int, const void *, int);
int compat_sigaction(int, const void *, void *);
int compat_sigaltstack(const void *, void *);
int compat_ioctl(int, int, char *);
int compat_sendmsg(int, const void *, int);
int compat_recvmsg(int, void *, int);
int compat_sendmmsg(int, void *, unsigned, int);
int compat_recvmmsg(int, void *, unsigned, int, void *);
#else
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
#endif
