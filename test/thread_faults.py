#!/usr/bin/env python3
"""Validate caught faults, thread-group termination, clocks, and trace detachment."""
from pathlib import Path
import os
import subprocess
import tempfile

PROBE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static volatile sig_atomic_t recovered, injected;
static uintptr_t alternate_begin, alternate_end;
static int use_alternate;

static void fault(void)
{
#if defined(__x86_64__)
    __asm__ volatile("xor %%eax, %%eax; .byte 0xc6, 0x00, 0x00" ::: "rax", "memory");
#elif defined(__i386__)
    __asm__ volatile("xor %%eax, %%eax; .byte 0xc6, 0x00, 0x00" ::: "eax", "memory");
#else
#error x86 register context required
#endif
}

static void recover_fault(int sig, siginfo_t *info, void *context)
{
    volatile unsigned char local;
    ucontext_t *uc = context;
    if (sig != SIGSEGV || info->si_code != SEGV_MAPERR || info->si_addr != NULL)
        _exit(90);
    if (uc->uc_mcontext.gregs[REG_TRAPNO] != 14 ||
        uc->uc_mcontext.gregs[REG_ERR] != 6)
        _exit(91);
#if defined(__x86_64__)
    if (uc->uc_mcontext.gregs[REG_CR2] != 0)
#else
    if (uc->uc_mcontext.cr2 != 0)
#endif
        _exit(91);
    if (use_alternate && ((uintptr_t)&local < alternate_begin ||
                          (uintptr_t)&local >= alternate_end))
        _exit(92);
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RIP] += 3;
#else
    uc->uc_mcontext.gregs[REG_EIP] += 3;
#endif
    recovered++;
}

static int wait_bounded(pid_t child, int options)
{
    for (unsigned i = 0; i < 500; i++) {
        int status;
        pid_t result = waitpid(child, &status, options | WNOHANG);
        assert(result >= 0);
        if (result == child)
            return status;
        usleep(10000);
    }
    kill(child, SIGKILL);
    fprintf(stderr, "Child %d did not report termination or stop within five seconds\n", child);
    exit(1);
}

static void *worker(void *argument)
{
    int mode = (intptr_t)argument;
    if (mode == 1)
        _exit(37);
    if (mode == 2)
        syscall(SYS_tkill, syscall(SYS_gettid), SIGKILL);
    fault();
    _exit(93);
}

static void group_termination(int mode)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct sigaction action = { .sa_handler = mode == 3 ? SIG_IGN : SIG_DFL };
        assert(!sigaction(SIGSEGV, &action, NULL));
        if (mode == 4) {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGSEGV);
            assert(!pthread_sigmask(SIG_BLOCK, &set, NULL));
        }
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, worker, (void *)(intptr_t)mode));
        for (;;)
            pause();
    }
    int status = wait_bounded(child, 0);
    if (mode == 1)
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 37);
    else
        assert(WIFSIGNALED(status) && WTERMSIG(status) == (mode == 2 ? SIGKILL : SIGSEGV));
}

static void *ordinary_worker(void *argument) { return argument; }
static void injected_signal(int sig) { injected = sig; }

static void detach_check(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct sigaction action = { .sa_handler = injected_signal };
        assert(!sigaction(SIGUSR1, &action, NULL));
        assert(!ptrace(PTRACE_TRACEME, 0, NULL, NULL));
        raise(SIGSTOP);
        if (injected != SIGUSR1 || ptrace(PTRACE_TRACEME, 0, NULL, NULL))
            _exit(94);
        _exit(23);
    }
    int status = wait_bounded(child, WUNTRACED);
    assert(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    assert(!ptrace(PTRACE_DETACH, child, NULL, (void *)(intptr_t)SIGUSR1));
    status = wait_bounded(child, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 23);
}

int main(void)
{
    alarm(30);
    const clockid_t clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW,
                              CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME};
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        struct timespec res = {-1, -1};
        assert(!clock_getres(clocks[i], &res));
        assert(res.tv_sec >= 0 && res.tv_nsec >= 0 && res.tv_nsec < 1000000000);
        assert(res.tv_sec || res.tv_nsec);
        assert(!clock_getres(clocks[i], NULL));
    }
    errno = 0;
    assert(clock_getres(-1, NULL) == -1 && errno == EINVAL);
    assert(!syscall(SYS_tkill, syscall(SYS_gettid), 0));
    struct sigaction action = {.sa_sigaction = recover_fault, .sa_flags = SA_SIGINFO};
    assert(!sigaction(SIGSEGV, &action, NULL));
    fault();
    assert(recovered == 1);
    void *memory = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    stack_t alternate = {.ss_sp = memory, .ss_size = 65536};
    assert(!sigaltstack(&alternate, NULL));
    alternate_begin = (uintptr_t)memory;
    alternate_end = alternate_begin + 65536;
    use_alternate = 1;
    action.sa_flags |= SA_ONSTACK;
    assert(!sigaction(SIGSEGV, &action, NULL));
    fault();
    assert(recovered == 2);
    alternate.ss_flags = SS_DISABLE;
    assert(!sigaltstack(&alternate, NULL));
    assert(!munmap(memory, 65536));
    use_alternate = 0;
    pthread_t thread;
    void *result;
    assert(!pthread_create(&thread, NULL, ordinary_worker, (void *)7));
    assert(!pthread_join(thread, &result) && result == (void *)7);
    for (int mode = 0; mode <= 4; mode++)
        group_termination(mode);
    detach_check();
    puts("Thread fault, clock resolution, and trace detach checks: PASS");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="thread-faults-") as directory:
        root = Path(directory)
        source = root / "probe.c"
        executable = root / "probe"
        source.write_text(PROBE)
        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-Wall", "-Wextra", "-Werror",
                        "-pthread", str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=35)


if __name__ == "__main__":
    main()
