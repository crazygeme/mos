#!/bin/sh
# Validate FIFO descriptor lifetime and peer synchronization.
set -eu
probe_dir=$(mktemp -d /tmp/mos-fifo-test.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"%d: %s: %s\n",__LINE__,#x,strerror(errno));exit(1);} } while(0)
int main(void) {char name[80],fdname[80];snprintf(name,sizeof(name),"/tmp/mos-fifo-%d",getpid());CHECK(!mkfifo(name,0600));int path=open(name,O_PATH|O_CLOEXEC);CHECK(path>=0);errno=0;CHECK(open(name,O_WRONLY|O_NONBLOCK)==-1&&errno==ENXIO);CHECK(!unlink(name));snprintf(fdname,sizeof(fdname),"/proc/self/fd/%d",path);pid_t child=fork();CHECK(child>=0);if(!child){int r=open(fdname,O_RDONLY);CHECK(r>=0);char b=0;CHECK(read(r,&b,1)==1&&b=='X');struct pollfd event={.fd=r,.events=POLLIN}; CHECK(poll(&event,1,1000)==1&&(event.revents&POLLHUP)); CHECK(read(r,&b,1)==0);close(r);_exit(0);}int w=open(fdname,O_WRONLY);CHECK(w>=0);CHECK(write(w,"X",1)==1);close(w);int st;CHECK(waitpid(child,&st,0)==child&&WIFEXITED(st)&&!WEXITSTATUS(st));close(path);puts("FIFO descriptor reopen and peer handshake: PASS");return 0;}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
