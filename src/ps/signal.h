/*
 * signal.h — POSIX signal numbers, sigaction, and signal delivery frame.
 */

#ifndef _PS_SIGNAL_H
#define _PS_SIGNAL_H

#include <arch/types.h>
#include <arch/signal.h>

/* -----------------------------------------------------------------------
 * Signal numbers (Linux/i386 ABI)
 * --------------------------------------------------------------------- */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31

#define NSIG 32 /* one past the highest signal number */

/* Signal mask type: one bit per signal (bit N-1 = signal N) */
typedef unsigned long sigset_t;

/* Special sa_handler values */
#define SIG_DFL ((void (*)(int))0) /* default action  */
#define SIG_IGN ((void (*)(int))1) /* ignore          */
#define SIG_ERR ((void (*)(int)) - 1) /* error return    */

/* sa_flags bits */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO 0x00000004
#define SA_RESTORER 0x04000000
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000

/* sigprocmask(2) how-values */
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* sigaltstack ss_flags */
#define SS_ONSTACK 1 /* process is executing on the altstack */
#define SS_DISABLE 2 /* altstack is disabled */
#define MINSIGSTKSZ 2048

typedef struct {
	void *ss_sp;
	int ss_flags;
	unsigned ss_size;
} stack_t;

struct sigaction {
	void (*sa_handler)(int);
	sigset_t sa_mask;
	int sa_flags;
	void (*sa_restorer)(void);
};

/* The concrete user-stack signal-frame layout is supplied by the architecture. */
#endif /* _SIGNAL_H */
