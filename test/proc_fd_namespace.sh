#!/bin/sh
# Validate descriptor lookup within a PID namespace.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc-fd.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <sched.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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
#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef SYS_openat
#if defined(__i386__)
#define SYS_openat 295
#define SYS_readlinkat 305
#else
#define SYS_openat 257
#define SYS_readlinkat 267
#endif
#endif
#define openat(fd, path, flags) syscall(SYS_openat, (fd), (path), (flags), 0)
#define readlinkat(fd, path, buf, size) syscall(SYS_readlinkat, (fd), (path), (buf), (size))
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno));exit(1);}}while(0)
int main(void) {CHECK(!unshare(CLONE_NEWPID|CLONE_NEWNS));pid_t child=fork();CHECK(child>=0);if(!child){CHECK(getpid()==1);int fd=open("/proc/self/fd",O_PATH|O_DIRECTORY);CHECK(fd>=0);int root=open("/tmp",O_PATH|O_DIRECTORY);CHECK(root>=0);char n[20]; char *buf=malloc(4096); CHECK(buf);snprintf(n,sizeof(n),"%d",root);int link=openat(fd,n,O_PATH|O_NOFOLLOW);CHECK(link>=0);ssize_t count=readlinkat(link,"",buf,4095);CHECK(count>0);buf[count]=0;CHECK(!strcmp(buf,"/tmp"));int reopened=openat(fd,n,O_RDONLY|O_DIRECTORY);CHECK(reopened>=0);struct stat st;CHECK(!fstat(reopened,&st)&&S_ISDIR(st.st_mode));close(reopened);close(link);close(root);close(fd);free(buf);_exit(0);}int st;CHECK(waitpid(child,&st,0)==child&&WIFEXITED(st)&&!WEXITSTATUS(st));puts("proc descriptor lookup inside PID namespace: PASS");return 0;}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
