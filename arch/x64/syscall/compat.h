#ifndef MOS_X64_SYSCALL_COMPAT_H
#define MOS_X64_SYSCALL_COMPAT_H
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
#endif
