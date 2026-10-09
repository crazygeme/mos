#!/bin/sh
# Check directory-only opens and copy destination classification.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-directory_open.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC O_CLOEXEC
#define SOCK_NONBLOCK O_NONBLOCK
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif
#define CHECK(x)                                                            \
	do {                                                                \
		if (!(x)) {                                                 \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, \
				__LINE__, #x, errno);                       \
			exit(1);                                            \
		}                                                           \
	} while (0)

int main(void)
{
	const char *files[] = { "destination", "file-link", "fifo" },
		   *dirs[] = { "directory", "directory-link" };
	int i, j, fd;
	struct stat st;
	CHECK(!mkdir("directory", 0700));
	CHECK((fd = open("destination", O_CREAT | O_WRONLY, 0600)) >= 0);
	close(fd);
	CHECK(!symlink("destination", "file-link") &&
	      !symlink("directory", "directory-link") && !mkfifo("fifo", 0600));
	for (i = 0; i < 2; i++) {
		int flags = O_DIRECTORY |
			    (i ? 010000000 : O_RDONLY | O_NONBLOCK);
		for (j = 0; j < 3; j++) {
			CHECK(open(files[j], flags) == -1 && errno == ENOTDIR);
		}
		for (j = 0; j < 2; j++) {
			CHECK((fd = open(dirs[j], flags)) >= 0);
			CHECK(!fstat(fd, &st) && S_ISDIR(st.st_mode));
			close(fd);
		}
	}
	unlink("file-link");
	unlink("directory-link");
	unlink("fifo");
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
cd "$probe_dir"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
printf 'new payload\n' > source
printf 'old payload\n' > destination
cp source destination
cmp source destination
cp source directory
cmp source directory/source
