#!/bin/sh
# Validate shared mappings across writes, file growth, and mount namespaces.
set -eu
probe_dir=$(mktemp -d /tmp/mos-shared-write.XXXXXX)
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
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#ifndef SYS_unshare
#if defined(__i386__)
#define SYS_unshare 310
#else
#define SYS_unshare 272
#endif
#endif
#define unshare(flags) syscall(SYS_unshare, (flags))
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while(0)
int main(void) {
 for(int i=0;i<16;i++) { char reused[]="/tmp/mos-inode-reuse-XXXXXX"; int fresh=mkstemp(reused); CHECK(fresh>=0&&ftruncate(fresh,4096)==0); unsigned char *page=mmap(0,4096,PROT_READ,MAP_SHARED,fresh,0); CHECK(page!=MAP_FAILED&&page[32]==0); CHECK(pwrite(fresh,"reuse",5,32)==5&&!memcmp(page+32,"reuse",5)); CHECK(munmap(page,4096)==0); close(fresh); CHECK(unlink(reused)==0); }
 char name[]="/tmp/mos-shared-write-XXXXXX"; int fd=mkstemp(name); CHECK(fd>=0); CHECK(ftruncate(fd,4096)==0);
 unsigned char *map=mmap(0,4096,PROT_READ,MAP_SHARED,fd,0); CHECK(map!=MAP_FAILED&&map[32]==0);
 CHECK(pwrite(fd,"one",3,32)==3); CHECK(!memcmp(map+32,"one",3));
 CHECK(ftruncate(fd,8192)==0); CHECK(pwrite(fd,"two",3,32)==3); CHECK(!memcmp(map+32,"two",3));
 pid_t pid=fork(); CHECK(pid>=0); if(!pid) { CHECK(unshare(CLONE_NEWNS)==0); int other=open(name,O_RDWR); CHECK(other>=0); CHECK(pwrite(other,"tri",3,32)==3); close(other); _exit(0); }
 int status; CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status)); CHECK(!memcmp(map+32,"tri",3));
 CHECK(munmap(map,4096)==0); close(fd); unlink(name); puts("shared file write coherence: PASS"); return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
