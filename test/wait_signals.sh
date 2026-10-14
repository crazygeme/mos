#!/bin/sh
# Validate wait4 results with a SIGCHLD handler.
set -eu
probe_dir=$(mktemp -d /tmp/mos-kernel-probe.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
static volatile sig_atomic_t received;
static void handler(int signo) {received++;}
int main(void) {struct sigaction a={.sa_handler=handler,.sa_flags=SA_RESTART};sigemptyset(&a.sa_mask);if(sigaction(SIGCHLD,&a,0))return 1;for(int i=0;i<100;i++){pid_t p=fork();if(!p)_exit(42);if(p<0)return 2;int st;struct rusage u;pid_t r=wait4(p,&st,0,&u);if(r!=p||!WIFEXITED(st)||WEXITSTATUS(st)!=42){fprintf(stderr,"pid=%d return=%d status=%d errno=%s\n",p,r,st,strerror(errno));return 3;}}printf("wait4 with SIGCHLD handler: PASS (%d signals)\n",received);return 0;}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
