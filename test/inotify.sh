#!/bin/sh
# Validate filesystem notifications, watch lifetime, queues, and proc controls.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-inotify.XXXXXX)
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

#include <sys/mount.h>

#define ACCESS 1
#define MODIFY 2
#define ATTRIB 4
#define CLOSE_WRITE 8
#define CLOSE_NOWRITE 16
#define OPEN 32
#define MOVED_FROM 64
#define MOVED_TO 128
#define CREATE 256
#define DELETE 512
#define DELETE_SELF 1024
#define MOVE_SELF 2048
#define UNMOUNT 0x2000
#define Q_OVERFLOW 0x4000
#define IGNORED 0x8000
#define ONLYDIR 0x1000000
#define DONT_FOLLOW 0x2000000
#define EXCL_UNLINK 0x4000000
#define MASK_CREATE 0x10000000
#define MASK_ADD 0x20000000
#define ISDIR 0x40000000
#define ONESHOT 0x80000000
#define ALL 0xfff
struct event {
	int wd;
	uint32_t mask, cookie, len;
};
static unsigned char events[131072];
static size_t event_size;
static int create(int flags)
{
	int fd = syscall(332, flags);
	CHECK(fd >= 0);
	return fd;
}
static int watch(int fd, const char *path, unsigned mask)
{
	int wd = syscall(292, fd, path, mask);
	CHECK(wd >= 0);
	return wd;
}
static void remove_watch(int fd, int wd)
{
	CHECK(!syscall(293, fd, wd));
}
static void decode(void)
{
	size_t offset = 0;
	struct event *e;
	while (offset < event_size) {
		CHECK(event_size - offset >= 16);
		e = (void *)(events + offset);
		CHECK(e->len % 16 == 0 && offset + 16 + e->len <= event_size);
		if (e->len)
			CHECK(memchr(events + offset + 16, 0, e->len));
		offset += 16 + e->len;
	}
}
static void drain(int fd, int append)
{
	ssize_t n;
	if (!append)
		event_size = 0;
	while ((n = read(fd, events + event_size,
			 sizeof(events) - event_size)) > 0) {
		event_size += n;
		CHECK(event_size < sizeof(events));
	}
	CHECK(n == -1 && errno == EAGAIN);
	decode();
}
static int has(int wd, unsigned mask, const char *name)
{
	size_t offset;
	struct event *e;
	for (offset = 0; offset < event_size; offset += 16 + e->len) {
		e = (void *)(events + offset);
		if (e->wd == wd && (e->mask & mask) == mask &&
		    !strcmp(e->len ? (char *)e + 16 : "", name))
			return 1;
	}
	return 0;
}
static int any(int wd, unsigned mask)
{
	size_t offset;
	struct event *e;
	for (offset = 0; offset < event_size; offset += 16 + e->len) {
		e = (void *)(events + offset);
		if ((wd == -2 || e->wd == wd) && (e->mask & mask))
			return 1;
	}
	return 0;
}
static void contains(int wd, unsigned mask, const char *name)
{
	CHECK(has(wd, mask, name));
}
static int queued(int fd)
{
	int n;
	CHECK(!ioctl(fd, FIONREAD, &n));
	return n;
}
static void touch(const char *path)
{
	int fd = open(path, O_CREAT | O_WRONLY, 0600);
	CHECK(fd >= 0);
	CHECK(!close(fd));
}
static void filesystem(void)
{
	int fd = create(O_NONBLOCK | O_CLOEXEC), wd, file, file_wd, held,
	    replacement_wd;
	unsigned cookie = 0;
	size_t offset;
	struct event *e;
	char b[3];
	CHECK(!mkdir("basic", 0700));
	CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) &&
	      (fcntl(fd, F_GETFD) & FD_CLOEXEC));
	CHECK(lseek(fd, 1, SEEK_SET) == 0 && !queued(fd));
	CHECK(read(fd, b, sizeof(b)) == -1 && errno == EAGAIN);
	wd = watch(fd, "basic", ALL);
	CHECK((file = open("basic/file", O_CREAT | O_RDWR, 0600)) >= 0);
	drain(fd, 0);
	CHECK(!any(-2, ATTRIB));
	CHECK(write(file, "abc", 3) == 3 && lseek(file, 0, SEEK_SET) == 0 &&
	      read(file, b, 3) == 3 && !memcmp(b, "abc", 3));
	CHECK(!fchmod(file, 0640) && !ftruncate(file, 1));
	close(file);
	drain(fd, 1);
	contains(wd, CREATE, "file");
	contains(wd, OPEN, "file");
	contains(wd, MODIFY, "file");
	contains(wd, ACCESS, "file");
	contains(wd, ATTRIB, "file");
	contains(wd, CLOSE_WRITE, "file");
	CHECK((file = open("basic/file", O_RDONLY)) >= 0);
	CHECK(read(file, b, 3) == 1);
	close(file);
	drain(fd, 0);
	contains(wd, CLOSE_NOWRITE, "file");
	CHECK((file = open("basic/file", O_RDONLY)) >= 0);
	drain(fd, 0);
	CHECK(ftruncate(file, 0) == -1 && errno == EINVAL);
	drain(fd, 0);
	CHECK(!event_size);
	close(file);
	drain(fd, 0);
	CHECK(!mkdir("basic/dir", 0700));
	drain(fd, 0);
	contains(wd, CREATE | ISDIR, "dir");
	touch("basic/dir/nested");
	drain(fd, 0);
	CHECK(!event_size);
	unlink("basic/dir/nested");
	CHECK(!rmdir("basic/dir"));
	drain(fd, 0);
	contains(wd, DELETE | ISDIR, "dir");
	file_wd = watch(fd, "basic/file", ALL);
	CHECK((file = open("basic/file", O_RDWR)) >= 0);
	drain(fd, 0);
	CHECK(!rename("basic/file", "basic/renamed"));
	drain(fd, 0);
	contains(wd, MOVED_FROM, "file");
	contains(wd, MOVED_TO, "renamed");
	contains(file_wd, MOVE_SELF, "");
	for (offset = 0; offset < event_size; offset += 16 + e->len) {
		e = (void *)(events + offset);
		if (e->wd == wd && (e->mask & MOVED_FROM))
			cookie = e->cookie;
	}
	CHECK(cookie);
	for (offset = 0; offset < event_size; offset += 16 + e->len) {
		e = (void *)(events + offset);
		if (e->wd == wd && (e->mask & MOVED_TO))
			CHECK(e->cookie == cookie);
	}
	CHECK(write(file, "x", 1) == 1);
	close(file);
	drain(fd, 0);
	contains(file_wd, MODIFY, "");
	contains(wd, CLOSE_WRITE, "renamed");
	CHECK(!link("basic/renamed", "basic/alias"));
	drain(fd, 0);
	contains(file_wd, ATTRIB, "");
	CHECK(watch(fd, "basic/alias",
		    MODIFY | DELETE_SELF | ATTRIB | CLOSE_WRITE) == file_wd);
	CHECK(syscall(292, fd, "basic/alias", MODIFY | MASK_CREATE) == -1 &&
	      errno == EEXIST);
	CHECK(!unlink("basic/renamed"));
	drain(fd, 0);
	contains(file_wd, ATTRIB, "");
	CHECK(!any(file_wd, IGNORED));
	CHECK((file = open("basic/alias", O_RDWR)) >= 0);
	drain(fd, 0);
	CHECK(!unlink("basic/alias"));
	drain(fd, 0);
	contains(file_wd, ATTRIB, "");
	CHECK(!any(file_wd, DELETE_SELF | IGNORED));
	close(file);
	drain(fd, 0);
	contains(file_wd, CLOSE_WRITE, "");
	contains(file_wd, DELETE_SELF, "");
	contains(file_wd, IGNORED, "");
	CHECK(syscall(293, fd, file_wd) == -1 && errno == EINVAL);
	touch("basic/old");
	touch("basic/replacement");
	replacement_wd = watch(fd, "basic/replacement", ALL);
	CHECK((held = open("basic/replacement", O_RDONLY)) >= 0);
	drain(fd, 0);
	CHECK(!rename("basic/old", "basic/replacement"));
	drain(fd, 0);
	contains(replacement_wd, ATTRIB, "");
	CHECK(!any(replacement_wd, IGNORED));
	close(held);
	drain(fd, 0);
	contains(replacement_wd, DELETE_SELF, "");
	contains(replacement_wd, IGNORED, "");
	remove_watch(fd, wd);
	drain(fd, 0);
	contains(wd, IGNORED, "");
	close(fd);
}
static void references(void)
{
	int fd, wd, held, action;
	const char *name;
	touch("path-reference");
	fd = create(O_NONBLOCK | O_CLOEXEC);
	wd = watch(fd, "path-reference", ALL);
	CHECK((held = open("path-reference", 010000000)) >= 0);
	drain(fd, 0);
	CHECK(!event_size);
	unlink("path-reference");
	drain(fd, 0);
	contains(wd, ATTRIB, "");
	CHECK(!any(-2, DELETE_SELF | IGNORED));
	close(held);
	drain(fd, 0);
	contains(wd, DELETE_SELF, "");
	contains(wd, IGNORED, "");
	CHECK(!any(-2, CLOSE_WRITE | CLOSE_NOWRITE));
	close(fd);
	CHECK(!mkdir("late-watch", 0700));
	for (action = 0; action < 2; action++) {
		touch("late-watch/item");
		CHECK((held = open("late-watch/item", O_RDWR)) >= 0);
		if (!action) {
			CHECK(!rename("late-watch/item", "late-watch/renamed"));
			name = "renamed";
		} else {
			CHECK(!unlink("late-watch/item"));
			name = "item";
		}
		fd = create(O_NONBLOCK | O_CLOEXEC);
		wd = watch(fd, "late-watch", MODIFY | CLOSE_WRITE);
		drain(fd, 0);
		CHECK(!event_size);
		CHECK(write(held, "x", 1) == 1);
		close(held);
		drain(fd, 0);
		contains(wd, MODIFY, name);
		contains(wd, CLOSE_WRITE, name);
		CHECK(!any(-2, Q_OVERFLOW));
		close(fd);
	}
}
static volatile sig_atomic_t received;
static void sigio(int sig)
{
	received = 1;
}
static void async_check(void)
{
	int fd, wd, i;
	CHECK(!mkdir("async", 0700));
	fd = create(O_NONBLOCK | O_CLOEXEC);
	signal(SIGIO, sigio);
	wd = watch(fd, "async", CREATE);
	CHECK(!fcntl(fd, F_SETOWN, getpid()));
	CHECK(!fcntl(fd, F_SETFL, O_NONBLOCK | O_ASYNC));
	touch("async/signal");
	for (i = 0; i < 200 && !received; i++)
		usleep(10000);
	CHECK(received);
	drain(fd, 0);
	contains(wd, CREATE, "signal");
	close(fd);
	signal(SIGIO, SIG_DFL);
}
static void masks(void)
{
	int fd, wd, ordinary, item_wd, link_wd, exclude, held;
	size_t offset;
	struct event *e;
	CHECK(!mkdir("masks", 0700));
	fd = create(O_NONBLOCK | O_CLOEXEC);
	CHECK(syscall(292, fd, "masks", 0) == -1 && errno == EINVAL);
	CHECK(syscall(292, fd, "masks", 0x1000) == -1 && errno == EINVAL);
	CHECK(syscall(292, fd, "masks", CREATE | MASK_ADD | MASK_CREATE) ==
		      -1 &&
	      errno == EINVAL);
	CHECK(syscall(292, -1, "masks", ALL) == -1 && errno == EBADF);
	CHECK(syscall(292, fd, "masks/missing", ALL) == -1 && errno == ENOENT);
	CHECK(syscall(292, fd, "", ALL) == -1 && errno == ENOENT);
	CHECK(syscall(292, fd, 0, ALL) == -1 && errno == EFAULT);
	ordinary = open("masks", O_RDONLY | O_DIRECTORY);
	CHECK(ordinary >= 0);
	CHECK(syscall(292, ordinary, "masks", ALL) == -1 && errno == EINVAL);
	close(ordinary);
	wd = watch(fd, "masks", CREATE);
	CHECK(watch(fd, "masks", DELETE | MASK_ADD) == wd);
	touch("masks/item");
	unlink("masks/item");
	drain(fd, 0);
	contains(wd, CREATE, "item");
	contains(wd, DELETE, "item");
	CHECK(watch(fd, "masks", OPEN) == wd);
	touch("masks/item");
	drain(fd, 0);
	CHECK(event_size);
	for (offset = 0; offset < event_size; offset += 16 + e->len) {
		e = (void *)(events + offset);
		CHECK(e->mask & OPEN);
	}
	CHECK(syscall(292, fd, "masks/item", MODIFY | ONLYDIR) == -1 &&
	      errno == ENOTDIR);
	CHECK(!symlink("item", "masks/link"));
	item_wd = watch(fd, "masks/item", ALL);
	CHECK(watch(fd, "masks/link", ALL) == item_wd);
	link_wd = watch(fd, "masks/link", ALL | DONT_FOLLOW);
	CHECK(link_wd != item_wd);
	drain(fd, 0);
	unlink("masks/link");
	drain(fd, 0);
	contains(link_wd, IGNORED, "");
	remove_watch(fd, wd);
	drain(fd, 0);
	wd = watch(fd, "masks", CREATE | ONESHOT);
	touch("masks/once");
	touch("masks/twice");
	drain(fd, 0);
	contains(wd, CREATE, "once");
	contains(wd, IGNORED, "");
	CHECK(!has(wd, CREATE, "twice"));
	CHECK(syscall(293, fd, wd) == -1 && errno == EINVAL);
	close(fd);
	for (exclude = 0; exclude < 2; exclude++) {
		fd = create(O_NONBLOCK | O_CLOEXEC);
		touch("masks/unlinked");
		wd = watch(fd, "masks",
			   CLOSE_WRITE | (exclude ? EXCL_UNLINK : 0));
		CHECK((held = open("masks/unlinked", O_RDWR)) >= 0);
		unlink("masks/unlinked");
		close(held);
		drain(fd, 0);
		CHECK(!!any(wd, CLOSE_WRITE) != exclude);
		close(fd);
	}
}
struct ep_event {
	uint32_t events;
	uint64_t data;
} __attribute__((packed));
static void queues(void)
{
	int fd, duplicate, wd, ep, count, file;
	struct ep_event e, out;
	struct pollfd p;
	struct iovec v[2];
	char b[65536];
	ssize_t n;
	CHECK(!mkdir("queues", 0700));
	fd = create(O_NONBLOCK | O_CLOEXEC);
	CHECK((duplicate = dup(fd)) >= 0);
	wd = watch(fd, "queues", CREATE);
	CHECK((ep = syscall(254, 1)) >= 0);
	e.events = 1;
	e.data = fd;
	CHECK(!syscall(255, ep, 1, fd, &e));
	CHECK(!syscall(256, ep, &out, 1, 0));
	touch("queues/a");
	CHECK(syscall(256, ep, &out, 1, 1000) == 1 && out.events == 1 &&
	      out.data == fd);
	p.fd = fd;
	p.events = POLLIN;
	CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
	count = queued(fd);
	CHECK(count == 32);
	CHECK(read(fd, b, 16) == -1 && errno == EINVAL && queued(fd) == count);
	v[0].iov_base = b;
	v[0].iov_len = 7;
	v[1].iov_base = b + 7;
	v[1].iov_len = count;
	CHECK(readv(duplicate, v, 2) == -1 && errno == EINVAL &&
	      queued(fd) == count);
	v[0].iov_base = events;
	v[0].iov_len = count;
	v[1].iov_len = 0;
	CHECK(readv(duplicate, v, 2) == count);
	event_size = count;
	decode();
	contains(wd, CREATE, "a");
	CHECK(!queued(fd) && !syscall(256, ep, &out, 1, 0));
	touch("queues/b");
	touch("queues/c");
	CHECK((n = read(fd, events, 32)) == 32);
	event_size = n;
	decode();
	contains(wd, CREATE, "b");
	drain(duplicate, 0);
	contains(wd, CREATE, "c");
	close(ep);
	close(fd);
	touch("queues/d");
	drain(duplicate, 0);
	contains(wd, CREATE, "d");
	close(duplicate);
	touch("queues/coalesced");
	fd = create(O_NONBLOCK | O_CLOEXEC);
	CHECK((file = open("queues/coalesced", O_WRONLY)) >= 0);
	wd = watch(fd, "queues/coalesced", MODIFY);
	CHECK(write(file, "a", 1) == 1 && write(file, "b", 1) == 1);
	drain(fd, 0);
	CHECK(event_size == 16);
	contains(wd, MODIFY, "");
	close(file);
	close(fd);
}
static void waits(void)
{
	int fd, wd, status;
	pid_t child;
	ssize_t n;
	CHECK(!mkdir("waits", 0700));
	CHECK((fd = syscall(291)) >= 0);
	CHECK(!(fcntl(fd, F_GETFL) & O_NONBLOCK) &&
	      !(fcntl(fd, F_GETFD) & FD_CLOEXEC));
	wd = watch(fd, "waits", CREATE);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		usleep(50000);
		touch("waits/wake");
		_exit(0);
	}
	CHECK((n = read(fd, events, sizeof(events))) > 0);
	event_size = n;
	decode();
	contains(wd, CREATE, "wake");
	child_ok(child);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		read(fd, events, sizeof(events));
		_exit(1);
	}
	usleep(50000);
	CHECK(!kill(child, SIGKILL));
	CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
	close(fd);
}
static long setting_read(const char *name)
{
	char path[128];
	long n;
	FILE *f;
	snprintf(path, sizeof(path), "/proc/sys/fs/inotify/%s", name);
	CHECK((f = fopen(path, "r")));
	CHECK(fscanf(f, "%ld", &n) == 1 && n >= 0);
	fclose(f);
	return n;
}
static void setting(const char *name, long n)
{
	char path[128], text[64];
	int fd, len;
	snprintf(path, sizeof(path), "/proc/sys/fs/inotify/%s", name);
	len = snprintf(text, sizeof(text), "%ld\n", n);
	CHECK((fd = open(path, O_WRONLY)) >= 0);
	CHECK(write(fd, text, len) == len);
	close(fd);
	CHECK(setting_read(name) == n);
}
static long saved[3];
static int restore_limits;
static const char *controls[] = { "max_user_watches", "max_user_instances",
				  "max_queued_events" };
static void restore(void)
{
	int i;
	if (restore_limits)
		for (i = 0; i < 3; i++)
			setting(controls[i], saved[i]);
}
static void limits(void)
{
	int i, fd, other, zero, wd, control, overflows = 0, count = 0;
	size_t offset;
	struct event *e;
	const char *bad[] = { "-1\n", "2147483648\n", "1x\n", "\n" };
	char path[64];
	for (i = 0; i < 3; i++)
		saved[i] = setting_read(controls[i]);
	restore_limits = 1;
	atexit(restore);
	CHECK(!mkdir("limits", 0700));
	CHECK((control = open("/proc/sys/fs/inotify/max_user_watches",
			      O_WRONLY)) >= 0);
	for (i = 0; i < 4; i++)
		CHECK(write(control, bad[i], strlen(bad[i])) == -1 &&
		      errno == EINVAL);
	close(control);
	fd = create(O_NONBLOCK | O_CLOEXEC);
	wd = watch(fd, "limits", CREATE);
	setting(controls[0], 0);
	CHECK(watch(fd, "limits", CREATE | MASK_ADD) == wd);
	touch("limits/watched");
	CHECK(syscall(292, fd, "limits/watched", ALL) == -1 && errno == ENOSPC);
	setting(controls[1], 0);
	CHECK(syscall(332, O_NONBLOCK | O_CLOEXEC) == -1 && errno == EMFILE);
	setting(controls[0], saved[0]);
	setting(controls[1], saved[1]);
	setting(controls[2], 4);
	other = create(O_NONBLOCK | O_CLOEXEC);
	wd = watch(other, "limits", CREATE);
	setting(controls[2], 0);
	for (i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "limits/%d", i);
		touch(path);
	}
	drain(other, 0);
	for (offset = 0; offset < event_size; offset += 16 + e->len) {
		e = (void *)(events + offset);
		count++;
		if (e->wd == -1 && e->mask == Q_OVERFLOW)
			overflows++;
	}
	CHECK(count == 5 && overflows == 1);
	touch("limits/after-overflow");
	drain(other, 0);
	contains(wd, CREATE, "after-overflow");
	zero = create(O_NONBLOCK | O_CLOEXEC);
	watch(zero, "limits", CREATE);
	touch("limits/zero-limit");
	drain(zero, 0);
	CHECK(event_size == 16);
	contains(-1, Q_OVERFLOW, "");
	close(zero);
	close(other);
	close(fd);
	restore();
	restore_limits = 0;
}
static void mounts(void)
{
	int fd, wd;
	CHECK(!mkdir("mounted", 0700));
	CHECK(!mount("tmpfs", "mounted", "tmpfs", 0, 0));
	fd = create(O_NONBLOCK | O_CLOEXEC);
	wd = watch(fd, "mounted", ALL);
	CHECK(!umount2("mounted", 0));
	drain(fd, 0);
	contains(wd, UNMOUNT, "");
	contains(wd, IGNORED, "");
	CHECK(syscall(293, fd, wd) == -1 && errno == EINVAL);
	close(fd);
}
int main(int argc, char **argv)
{
	int i, guest = 0, do_limits = 0, do_mounts = 0;
	const char *directory = 0;
	alarm(30);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--guest"))
			guest = 1;
		else if (!strcmp(argv[i], "--limits"))
			do_limits = 1;
		else if (!strcmp(argv[i], "--mounts"))
			do_mounts = 1;
		else if (!strcmp(argv[i], "--directory") && i + 1 < argc)
			directory = argv[++i];
		else
			CHECK(0);
	}
	CHECK(!(do_limits || do_mounts) || (guest && !geteuid()));
	if (directory) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/mos-inotify.XXXXXX",
			 directory);
		CHECK(mkdtemp(path) && !chdir(path));
	}
	CHECK(syscall(332, 1) == -1 && errno == EINVAL);
	for (i = 0; i < 3; i++)
		setting_read(controls[i]);
	filesystem();
	references();
	masks();
	queues();
	waits();
	async_check();
	if (do_limits)
		limits();
	if (do_mounts)
		mounts();
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe"
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
