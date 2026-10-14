#!/bin/sh
# Validate subreaper behavior.
set -eu
probe_dir=$(mktemp -d /tmp/mos-subreaper.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_CHILD_SUBREAPER 37
#endif
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno)); exit(1); } } while(0)
int main(void) {
 CHECK(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)==0); int value=0; CHECK(prctl(PR_GET_CHILD_SUBREAPER,&value,0,0,0)==0&&value==1);
 int pipefd[2]; CHECK(pipe(pipefd)==0); pid_t first=fork(); CHECK(first>=0);
 if(!first) { close(pipefd[0]); CHECK(prctl(PR_GET_CHILD_SUBREAPER,&value,0,0,0)==0&&value==0); pid_t second=fork(); CHECK(second>=0); if(!second) { close(pipefd[1]); usleep(200000); CHECK(getppid()!=first); _exit(42); } CHECK(write(pipefd[1],&second,sizeof(second))==sizeof(second)); _exit(0); }
 close(pipefd[1]); pid_t second; CHECK(read(pipefd[0],&second,sizeof(second))==sizeof(second)); close(pipefd[0]); int status;
 CHECK(waitpid(first,&status,0)==first&&WIFEXITED(status)&&!WEXITSTATUS(status)); CHECK(waitpid(second,&status,0)==second&&WIFEXITED(status)&&WEXITSTATUS(status)==42);
 puts("child subreaper adoption: PASS"); return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
