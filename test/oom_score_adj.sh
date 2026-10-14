#!/bin/sh
# Validate process OOM adjustment metadata and write permissions.
set -eu
probe_dir=$(mktemp -d /tmp/mos-oom-score.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while(0)
static int score(void) { FILE *f=fopen("/proc/self/oom_score_adj","r"); int value; CHECK(f&&fscanf(f,"%d",&value)==1); fclose(f); return value; }
static void set_score(const char *text) { int fd=open("/proc/self/oom_score_adj",O_WRONLY); CHECK(fd>=0); CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text)); close(fd); }
int main(void) { int saved=score(); set_score("250"); CHECK(score()==250); pid_t child=fork(); CHECK(child>=0); if(!child) { CHECK(score()==250); CHECK(setuid(1000)==0); set_score("300"); CHECK(score()==300); int fd=open("/proc/self/oom_score_adj",O_WRONLY); CHECK(fd>=0); errno=0; CHECK(write(fd,"200",3)==-1&&errno==EPERM); close(fd); _exit(0); } int s; CHECK(waitpid(child,&s,0)==child&&WIFEXITED(s)&&!WEXITSTATUS(s)); CHECK(score()==250); char text[16]; snprintf(text,sizeof(text),"%d",saved); set_score(text); puts("process OOM adjustment metadata: PASS"); return 0; }
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
