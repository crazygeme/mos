#!/bin/sh
# Validate realtime signals behavior.
set -eu
probe_dir=$(mktemp -d /tmp/mos-realtime_signals.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/syscall.h>
#ifndef CLONE_NEWPID
#define CLONE_NEWPID 0x20000000
#endif
#ifndef SYS_unshare
#if defined(__i386__)
#define SYS_unshare 310
#else
#define SYS_unshare 272
#endif
#endif
#define unshare(flags) syscall(SYS_unshare, (flags))
#ifndef SI_TKILL
#define SI_TKILL -6
#endif
#ifndef SYS_tgkill
#if defined(__i386__)
#define SYS_tgkill 270
#define SYS_signalfd4 327
#else
#define SYS_tgkill 234
#define SYS_signalfd4 289
#endif
#endif
#ifndef SYS_signalfd4
#if defined(__i386__)
#define SYS_signalfd4 327
#else
#define SYS_signalfd4 289
#endif
#endif
#define SFD_CLOEXEC 02000000
struct mos_signalfd_siginfo {
 uint32_t ssi_signo; int32_t ssi_errno, ssi_code;
 uint32_t ssi_pid; unsigned char remaining[112];
};
#define signalfd(fd, mask, flags) syscall(SYS_signalfd4, (fd), (mask), 8, (flags))
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while(0)
static volatile sig_atomic_t received;
static void handler(int signo, siginfo_t *info, void *context) { if(info->si_pid==getpid()&&info->si_code==SI_TKILL) received=signo; }
static int probe(void) {
 int signo=SIGRTMIN+7; struct sigaction action={.sa_sigaction=handler,.sa_flags=SA_SIGINFO}; sigemptyset(&action.sa_mask); CHECK(sigaction(signo,&action,0)==0);
 CHECK(syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),signo)==0); CHECK(received==signo);
 sigset_t mask; sigemptyset(&mask); sigaddset(&mask,signo); CHECK(sigprocmask(SIG_BLOCK,&mask,0)==0);
 int fd=signalfd(-1,&mask,SFD_CLOEXEC); CHECK(fd>=0); CHECK(kill(getpid(),signo)==0); struct mos_signalfd_siginfo info;
 CHECK(read(fd,&info,sizeof(info))==sizeof(info)&&info.ssi_signo==(unsigned)signo&&info.ssi_pid==(unsigned)getpid()); close(fd); puts("realtime signals and signalfd: PASS"); return 0;
}

int main(void) { CHECK(!unshare(CLONE_NEWPID)); pid_t child=fork(); CHECK(child>=0); if(!child) _exit(probe()); int st; CHECK(waitpid(child,&st,0)==child&&WIFEXITED(st)&&!WEXITSTATUS(st)); return 0; }
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
