#!/bin/sh
# Validate bind symlinks, read-only mounts, and shared mappings.
set -eu
probe_dir=$(mktemp -d /tmp/mos-kernel-probe.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#ifndef SYS_unshare
#if defined(__i386__)
#define SYS_unshare 310
#else
#define SYS_unshare 272
#endif
#endif
#define unshare(flags) syscall(SYS_unshare, (flags))
#ifndef MS_REC
#define MS_REC 16384
#endif
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno));exit(1);}}while(0)
int main(void) {char base[]="/tmp/mos-bind-ro.XXXXXX",src[128],dst[128],in[160],link[160],view[160],missing[160],dir[160],dev[160];CHECK(mkdtemp(base));snprintf(src,sizeof(src),"%s/src",base);snprintf(dst,sizeof(dst),"%s/dst",base);CHECK(!mkdir(src,0700)&&!mkdir(dst,0700));snprintf(in,sizeof(in),"%s/input",src);snprintf(link,sizeof(link),"%s/link",src);int f=open(in,O_CREAT|O_RDWR,0600);CHECK(f>=0&&write(f,"hello",5)==5&&ftruncate(f,4096)==0);close(f);CHECK(!symlink("input",link));CHECK(!unshare(CLONE_NEWNS));CHECK(!mount(src,dst,0,MS_BIND|MS_REC,0));snprintf(view,sizeof(view),"%s/link",dst);f=open(view,O_RDWR);CHECK(f>=0);char text[8]={0};CHECK(read(f,text,5)==5&&!memcmp(text,"hello",5));CHECK(!mount(0,dst,0,MS_REMOUNT|MS_BIND|MS_RDONLY,0));errno=0;CHECK(pwrite(f,"bad",3,0)==-1&&errno==EROFS);errno=0;CHECK(ftruncate(f,0)==-1&&errno==EROFS);errno=0;CHECK(fchmod(f,0400)==-1&&errno==EROFS);snprintf(missing,sizeof(missing),"%s/new",dst);errno=0;CHECK(open(missing,O_CREAT|O_WRONLY,0600)==-1&&errno==EROFS);snprintf(dir,sizeof(dir),"%s/newdir",dst);errno=0;CHECK(mkdir(dir,0700)==-1&&errno==EROFS);snprintf(dev,sizeof(dev),"%s/null",dst);errno=0;CHECK(mknod(dev,S_IFCHR|0600,makedev(1,3))==-1&&errno==EROFS);errno=0;CHECK(mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,f,0)==MAP_FAILED&&errno==EROFS);int ro=open(view,O_RDONLY);CHECK(ro>=0);void *map=mmap(0,4096,PROT_READ,MAP_SHARED,ro,0);CHECK(map!=MAP_FAILED);errno=0;CHECK(mprotect(map,4096,PROT_READ|PROT_WRITE)==-1&&errno==EACCES);int source=open(in,O_WRONLY);CHECK(source>=0&&write(source,"HELLO",5)==5);CHECK(!memcmp(map,"HELLO",5));close(source);CHECK(!munmap(map,4096));close(ro);close(f);CHECK(!umount(dst));CHECK(!unlink(link)&&!unlink(in)&&!rmdir(src)&&!rmdir(dst)&&!rmdir(base));puts("bind symlinks, readonly mounts and shared mappings: PASS");return 0;}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
