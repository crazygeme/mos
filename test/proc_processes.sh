#!/bin/sh
# Validate procfs directory types, ownership, parents, and libgtop selection.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_processes.XXXXXX)
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
static void child_ok(pid_t pid)
{
	int status;
	CHECK(pid > 0);
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

#include <dirent.h>

static long field(const char *path, const char *key)
{
	FILE *f = fopen(path, "r");
	char line[1024], name[80];
	long n;
	CHECK(f);
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%79[^:]: %ld", name, &n) == 2 &&
		    !strcmp(name, key)) {
			fclose(f);
			return n;
		}
	}
	fclose(f);
	CHECK(0);
	return -1;
}

#include <dlfcn.h>
struct summary {
	uint64_t flags, number, total, size;
};

static int *(*list)(struct summary *, int64_t, int64_t);
static void (*release)(void *);
static int *(*old_list)(void *, struct summary *, int64_t, int64_t);
static void **old_server;
static int *legacy_list(struct summary *s, int64_t which, int64_t arg)
{
	return old_list(*old_server, s, which, arg);
}
static void selection(int which, int arg, pid_t pid, int expected, int only)
{
	struct summary s;
	int *p, found = 0;
	uint64_t i;
	p = list(&s, which, arg);
	/* RH9 libgtop leaves the summary zeroed for an empty selection. */
	if (old_list && s.number == 0) {
		CHECK(!expected && !only && p == NULL);
		release(p);
		return;
	}
	CHECK((s.flags & 7) == 7 && s.size == sizeof(int) &&
	      s.total == s.number * s.size);
	for (i = 0; i < s.number; i++)
		if (p[i] == pid)
			found = 1;
	CHECK(found == expected);
	if (only)
		CHECK(s.number == 1);
	release(p);
}
int main(void)
{
	int capacity, fd, n, offset, ready[2], done[2], i, found;
	unsigned char buf[4096];
	unsigned short reclen;
	char *name, path[128], c, line[4096], *p;
	FILE *f;
	struct stat st;
	pid_t child;
	uid_t uid = geteuid() ? geteuid() : 1000;
	gid_t gid = geteuid() ? getegid() : 1000;
	void *gtop, *glib;
	alarm(10);
	for (capacity = 128; capacity <= 4096; capacity *= 32) {
		CHECK((fd = open("/proc", O_RDONLY | O_DIRECTORY)) >= 0);
		found = 0;
		while ((n = syscall(220, fd, buf, capacity)) > 0) {
			for (offset = 0; offset < n; offset += reclen) {
				CHECK(offset + 19 < n);
				memcpy(&reclen, buf + offset + 16, 2);
				CHECK(reclen >= 20 && offset + reclen <= n);
				name = (char *)buf + offset + 19;
				if (*name >= '0' && *name <= '9') {
					CHECK(buf[offset + 18] == 4);
					if (atoi(name) == getpid())
						found = 1;
				}
			}
		}
		CHECK(n == 0 && found);
		close(fd);
	}
	CHECK(field("/proc/1/status", "Pid") == 1 &&
	      !field("/proc/1/status", "PPid"));
	CHECK((f = fopen("/proc/1/stat", "r")));
	CHECK(fgets(line, sizeof(line), f));
	fclose(f);
	CHECK((p = strrchr(line, ')')) && atol(p + 4) == 0);

	glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
	CHECK(glib);
	*(void **)(&release) = dlsym(glib, "g_free");
	gtop = dlopen("libgtop-2.0.so.11", RTLD_NOW | RTLD_GLOBAL);
	if (gtop) {
		*(void **)(&list) = dlsym(gtop, "glibtop_get_proclist");
	} else {
		/* RH9 splits libgtop into mutually dependent libraries. */
		CHECK(dlopen("libgtop_sysdeps-2.0.so.0",
			     RTLD_LAZY | RTLD_GLOBAL));
		CHECK(dlopen("libgtop_names-2.0.so.0",
			     RTLD_LAZY | RTLD_GLOBAL));
		gtop = dlopen("libgtop-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
		CHECK(gtop);
		CHECK(dlopen("libgtop_common-2.0.so.0",
			     RTLD_LAZY | RTLD_GLOBAL));
		*(void **)(&old_list) = dlsym(gtop, "glibtop_get_proclist_l");
		old_server = dlsym(gtop, "glibtop_global_server");
		CHECK(old_list && old_server);
		list = legacy_list;
	}
	CHECK(list && release);
	CHECK(!pipe(ready) && !pipe(done));
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		close(ready[0]);
		close(done[1]);
		if (!geteuid())
			CHECK(!setegid(1000) && !seteuid(1000));
		CHECK(write(ready[1], "R", 1) == 1);
		read(done[0], &c, 1);
		_exit(0);
	}
	close(ready[1]);
	close(done[0]);
	CHECK(read(ready[0], &c, 1) == 1);
	for (i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "/proc/%d%s", child,
			 i == 0 ? "" :
			 i == 1 ? "/status" :
				  "/fd");
		CHECK(!stat(path, &st) && st.st_uid == uid && st.st_gid == gid);
		if (!i)
			CHECK(S_ISDIR(st.st_mode));
	}
	selection(0, 0, child, 1, 0);
	selection(1, child, child, 1, 1);
	selection(5, uid, child, 1, 0);
	selection(5, uid + 1, child, 0, 0);
	close(ready[0]);
	close(done[1]);
	child_ok(child);
	dlclose(gtop);
	dlclose(glib);
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe" -ldl
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
