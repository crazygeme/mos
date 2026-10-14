#!/bin/sh
# Validate tmpfs symlinks behavior.
set -eu
probe_dir=$(mktemp -d /tmp/mos-tmpfs_symlinks.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#ifndef AT_FDCWD
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#if defined(__i386__)
#define MOS_FSTATAT SYS_fstatat64
#ifndef SYS_fstatat64
#define SYS_fstatat64 300
#endif
#else
#define MOS_FSTATAT SYS_newfstatat
#endif
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while(0)
int main(void) {
 char base[128],path[256],link[256]; snprintf(base,sizeof(base),"/tmp/tmpfs-link-%d",getpid()); CHECK(mkdir(base,0700)==0); CHECK(mount("tmpfs",base,"tmpfs",0,0)==0);
 snprintf(path,sizeof(path),"%s/target",base); CHECK(mkdir(path,0700)==0);
 snprintf(path,sizeof(path),"%s/target/file",base); int fd=open(path,O_CREAT|O_WRONLY,0600); CHECK(fd>=0&&write(fd,"ok",2)==2); close(fd);
 snprintf(link,sizeof(link),"%s/alias",base); CHECK(symlink("target",link)==0); char value[256]; CHECK(readlink(link,value,sizeof(value))==6&&!memcmp(value,"target",6));
 snprintf(path,sizeof(path),"%s/alias/file",base); fd=open(path,O_RDONLY); CHECK(fd>=0&&read(fd,value,2)==2&&!memcmp(value,"ok",2)); close(fd);
 struct stat st; CHECK(lstat(link,&st)==0&&S_ISLNK(st.st_mode)); CHECK(stat(link,&st)==0&&S_ISDIR(st.st_mode));
 snprintf(path,sizeof(path),"%s/alias/file",base); struct stat64 at; CHECK(syscall(MOS_FSTATAT,AT_FDCWD,path,&at,AT_SYMLINK_NOFOLLOW)==0&&S_ISREG(at.st_mode));
 CHECK(unlink(link)==0); snprintf(path,sizeof(path),"%s/target/file",base); CHECK(unlink(path)==0); snprintf(path,sizeof(path),"%s/target",base); CHECK(rmdir(path)==0); CHECK(umount(base)==0); CHECK(rmdir(base)==0);
 puts("tmpfs symbolic links: PASS"); return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
