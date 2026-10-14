#!/bin/sh
# Validate capability namespace behavior.
set -eu
probe_dir=$(mktemp -d /tmp/mos-capability_namespace.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <linux/capability.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif
#ifndef SYS_unshare
#if defined(__i386__)
#define SYS_unshare 310
#else
#define SYS_unshare 272
#endif
#endif
#define unshare(flags) syscall(SYS_unshare, (flags))
#ifndef _LINUX_CAPABILITY_VERSION_3
#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#endif
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#endif
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while(0)
static void empty_caps(void) { struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3}; struct __user_cap_data_struct d[2]={{0},{0}}; CHECK(syscall(SYS_capset,&h,d)==0); }
static void no_caps(void) { struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3}; struct __user_cap_data_struct d[2]; CHECK(syscall(SYS_capget,&h,d)==0); CHECK(!d[0].effective&&!d[1].effective&&!d[0].permitted&&!d[1].permitted); errno=0; CHECK(unshare(CLONE_NEWUTS)==-1&&errno==EPERM); }
static void reap(pid_t pid) { int s; CHECK(waitpid(pid,&s,0)==pid&&WIFEXITED(s)&&!WEXITSTATUS(s)); }
int main(int argc,char **argv) {
 struct __user_cap_header_struct query={0}; CHECK(syscall(SYS_capget,&query,NULL)==0); CHECK(query.version==_LINUX_CAPABILITY_VERSION_3);
 if(argc>1) { CHECK(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==1); no_caps(); return 0; }
 pid_t child=fork(); CHECK(child>=0); if(!child) { struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3}; struct __user_cap_data_struct d[2]; CHECK(syscall(SYS_capget,&h,d)==0); CHECK(setuid(1000)==0); no_caps(); _exit(0); } reap(child);
 child=fork(); CHECK(child>=0); if(!child) { empty_caps(); no_caps(); CHECK(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0); execl(argv[0],argv[0],"check",NULL); CHECK(0); } reap(child);
 puts("capability drops and no_new_privs: PASS"); return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
