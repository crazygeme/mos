#!/bin/sh
# Validate process limits, device access, and cgroup namespace paths.
set -eu
probe_dir=$(mktemp -d /tmp/mos-cgroups.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif
#ifndef SYS_unshare
#if defined(__i386__)
#define SYS_unshare 310
#else
#define SYS_unshare 272
#endif
#endif
#define unshare(flags) syscall(SYS_unshare, (flags))
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while(0)
static void writefile(const char *path,const char *text) { int fd=open(path,O_WRONLY); CHECK(fd>=0); CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text)); CHECK(close(fd)==0); }
static void reap(pid_t child) { int s; CHECK(waitpid(child,&s,0)==child); CHECK(WIFEXITED(s)&&!WEXITSTATUS(s)); }
int main(void)
{
 CHECK(unshare(CLONE_NEWNS)==0);
 CHECK(mount("sysfs","/sys","sysfs",0,0)==0);
 CHECK(mount("tmpfs","/sys/fs/cgroup","tmpfs",0,0)==0);
 CHECK(mkdir("/sys/fs/cgroup/pids",0755)==0);
 CHECK(mkdir("/sys/fs/cgroup/devices",0755)==0);
 CHECK(mount("pids","/sys/fs/cgroup/pids","cgroup",0,"pids")==0);
 CHECK(mount("devices","/sys/fs/cgroup/devices","cgroup",0,"devices")==0);
 char pids[128],devices[128],path[256],pid[32];
 snprintf(pids,sizeof(pids),"/sys/fs/cgroup/pids/probe-%d",getpid());
 snprintf(devices,sizeof(devices),"/sys/fs/cgroup/devices/probe-%d",getpid());
 CHECK(mkdir(pids,0755)==0); CHECK(mkdir(devices,0755)==0);
 pid_t child=fork(); CHECK(child>=0);
 if(!child) {
  snprintf(pid,sizeof(pid),"%d",getpid());
  snprintf(path,sizeof(path),"%s/cgroup.procs",pids); writefile(path,pid);
  snprintf(path,sizeof(path),"%s/pids.max",pids); writefile(path,"1");
  errno=0; CHECK(fork()==-1&&errno==EAGAIN);
  writefile(path,"2"); pid_t inner=fork(); CHECK(inner>=0); if(!inner) _exit(0); reap(inner);
  snprintf(path,sizeof(path),"%s/cgroup.procs",devices); writefile(path,pid);
  CHECK(unshare(CLONE_NEWCGROUP)==0);
  int fd=open("/proc/self/cgroup",O_RDONLY); CHECK(fd>=0); char *buf=malloc(1024); CHECK(buf); int n=read(fd,buf,1023); CHECK(n>0); buf[n]=0; close(fd);
  CHECK(strstr(buf,"1:pids:/\n")&&strstr(buf,"2:devices:/\n")); free(buf);
  snprintf(path,sizeof(path),"%s/devices.deny",devices); writefile(path,"a *:* rwm");
  errno=0; CHECK(open("/dev/null",O_RDWR)==-1&&errno==EPERM);
  snprintf(path,sizeof(path),"%s/devices.allow",devices); writefile(path,"c 1:3 rw");
  fd=open("/dev/null",O_RDWR); CHECK(fd>=0); close(fd);
  errno=0; CHECK(mknod("/tmp/mos-cgroup-device",S_IFCHR|0600,0x103)==-1&&errno==EPERM);
  _exit(0);
 }
 reap(child); CHECK(rmdir(pids)==0); CHECK(rmdir(devices)==0);
 puts("cgroup pids, devices, and namespace: PASS");
 return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
