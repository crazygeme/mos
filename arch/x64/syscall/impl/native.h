#ifndef MOS_X64_NATIVE_SYSCALL_H
#define MOS_X64_NATIVE_SYSCALL_H

#include <syscall/syscall.h>
#include <int/int.h>

int native_sigaction(int, const void *, void *, unsigned);
intptr_t native_sigreturn(intr_frame *);
int native_sigaltstack(const void *, void *);
int native_stat_path(const char *, void *);
int native_stat_link(const char *, void *);
int native_stat_fd(int, void *);
int native_stat_at(int, const char *, void *, int);
int native_clock_gettime(int, void *);
int native_gettimeofday(void *, struct timezone *);
int native_nanosleep(const void *, void *);
int native_clock_nanosleep(int, int, const void *, void *);
int native_select(int, fd_set *, fd_set *, fd_set *, void *);
int native_pselect6(int, fd_set *, fd_set *, fd_set *, void *, const void *);
int native_ppoll(struct pollfd *, uintptr_t, void *, const sigset_t *,
		 uintptr_t);
int native_futex(int *, int, int, const void *, int *, int);
int native_rlimit(int, void *, int);
int native_wait4(int, int *, int, void *);
int native_getrusage(int, void *);
int native_fcntl(int, int, uintptr_t);
intptr_t native_arch_prctl(unsigned, uintptr_t);
intptr_t native_lseek(int, int64_t, int);
intptr_t native_mmap(vaddr_t, size_t, unsigned, unsigned, int, uint64_t);
intptr_t native_time(void *);
int native_utime(const char *, const void *);
int native_settimeofday(const void *, const struct timezone *);
int native_getitimer(int, void *);
int native_setitimer(int, const void *, void *);
int native_timer_create(int, const void *, int *);
int native_timer_gettime(int, void *);
int native_timer_settime(int, int, const void *, void *);
int native_sched_rr_get_interval(int, void *);
int native_sigpending(void *, unsigned);
int native_sigtimedwait(const sigset_t *, void *, const void *, unsigned);
int native_statfs(const char *, void *);
int native_fstatfs(int, void *);
int native_sysinfo(void *);
intptr_t native_times(void *);
int native_getdents(unsigned, void *, unsigned);
int native_socket(int, unsigned, int);
int native_socketpair(int, unsigned, int, int *);
struct sockaddr;
int native_accept4(int, struct sockaddr *, unsigned *, unsigned);
int native_sendto(int, const void *, size_t, int, const void *, unsigned);

#endif
