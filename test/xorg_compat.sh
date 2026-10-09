#!/bin/sh
# Run Xorg kernel-interface regression checks inside a MOS guest.
# The syscall probe uses the C compiler shipped with the guest.
set -eu
probe_dir=$(mktemp -d /tmp/mos-xorg_compat.XXXXXX)
cleanup()
{
    if [ "${sysfs_mounted:-0}" = 1 ]; then
        umount "$probe_dir/sysfs"
    fi
    rm -rf "$probe_dir"
}
trap cleanup 0
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

#include <dirent.h>
#include <dlfcn.h>
/* The old guest libc has no getauxval and procfs has no auxv file. */
extern char **environ;
static unsigned long aux(unsigned long key)
{
	char **env = environ;
	unsigned long *pair;
	while (*env)
		env++;
	pair = (unsigned long *)(env + 1);
	while (pair[0]) {
		if (pair[0] == key)
			return pair[1];
		pair += 2;
	}
	CHECK(0);
	return 0;
}
static void sockets(int abstract)
{
	struct sockaddr_un a, returned;
	int listener, duplicate, client, accepted;
	socklen_t len, actual;
	char b[3];
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	if (abstract) {
		snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "mos-xorg-%d",
			 getpid());
		len = 2 + 1 + strlen(a.sun_path + 1) + 5;
		memcpy(a.sun_path + len - 2 - 5, "\0name", 5);
	} else {
		CHECK(getcwd(a.sun_path, sizeof(a.sun_path)));
		CHECK(strlen(a.sun_path) + strlen("/xorg.socket") <
		      sizeof(a.sun_path));
		strcat(a.sun_path, "/xorg.socket");
		len = 2 + strlen(a.sun_path) + 1;
	}
	CHECK((listener = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
	CHECK(!bind(listener, (void *)&a, len) && !listen(listener, 1));
	actual = sizeof(returned);
	CHECK(!getsockname(listener, (void *)&returned, &actual) &&
	      actual == len && !memcmp(&returned, &a, len));
	CHECK((duplicate = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
	CHECK(bind(duplicate, (void *)&a, len) == -1 && errno == EADDRINUSE);
	close(duplicate);
	CHECK((client = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
	CHECK(!connect(client, (void *)&a, len));
	CHECK((accepted = accept(listener, 0, 0)) >= 0);
	actual = sizeof(returned);
	CHECK(!getpeername(client, (void *)&returned, &actual) &&
	      actual == len && !memcmp(&returned, &a, len));
	actual = sizeof(returned);
	CHECK(!getsockname(accepted, (void *)&returned, &actual) &&
	      actual == len && !memcmp(&returned, &a, len));
	CHECK(write(client, "X11", 3) == 3 && read(accepted, b, 3) == 3 &&
	      !memcmp(b, "X11", 3));
	close(accepted);
	close(client);
	close(listener);
	if (abstract) {
		CHECK((listener = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
		CHECK(!bind(listener, (void *)&a, len));
		close(listener);
	} else
		unlink(a.sun_path);
}
static void fence(void)
{
	char path[] = "/dev/shm/mos-fence.XXXXXX";
	int fd = mkstemp(path);
	unsigned char *a, *b;
	void *library, *map;
	int (*alloc)(void), (*query)(void *), (*trigger)(void *),
		(*reset)(void *);
	void *(*map_shm)(int);
	void (*unmap)(void *);
	CHECK(fd >= 0 && !unlink(path) && access(path, F_OK) == -1 &&
	      errno == ENOENT && !ftruncate(fd, 4096));
	a = mmap(0, 4096, 3, MAP_SHARED, fd, 0);
	b = mmap(0, 4096, 3, MAP_SHARED, fd, 0);
	CHECK(a != MAP_FAILED && b != MAP_FAILED);
	memcpy(a, "DRI3", 4);
	CHECK(!memcmp(b, "DRI3", 4));
	munmap(a, 4096);
	munmap(b, 4096);
	close(fd);
	CHECK(open("/dev/shm", 020000000 | O_DIRECTORY | O_RDWR, 0600) == -1 &&
	      errno == EOPNOTSUPP);
	library = dlopen("libxshmfence.so.1", RTLD_NOW);
	if (!library) {
		puts("SKIP: libxshmfence integration requires libxshmfence.so.1");
		return;
	}
	*(void **)(&alloc) = dlsym(library, "xshmfence_alloc_shm");
	*(void **)(&map_shm) = dlsym(library, "xshmfence_map_shm");
	*(void **)(&query) = dlsym(library, "xshmfence_query");
	*(void **)(&trigger) = dlsym(library, "xshmfence_trigger");
	*(void **)(&reset) = dlsym(library, "xshmfence_reset");
	*(void **)(&unmap) = dlsym(library, "xshmfence_unmap_shm");
	CHECK(alloc && map_shm && query && trigger && reset && unmap);
	CHECK((fd = alloc()) >= 0 && (map = map_shm(fd)));
	reset(map);
	CHECK(!query(map));
	trigger(map);
	CHECK(query(map));
	unmap(map);
	close(fd);
	dlclose(library);
}
static unsigned long hexfile(const char *path)
{
	FILE *f = fopen(path, "r");
	unsigned long n;
	CHECK(f && fscanf(f, "%lx", &n) == 1);
	fclose(f);
	return n;
}
static void pci(void)
{
	DIR *dir = opendir("sysfs/bus/pci/devices");
	struct dirent *e;
	char path[256], b[64], line[256];
	unsigned long klass;
	unsigned long long start, end, flags, rom_start, rom_end;
	int fd, mem, rows, devices = 0, displays = 0;
	uint32_t saved, enabled;
	FILE *f;
	CHECK(dir);
	while ((e = readdir(dir))) {
		if (e->d_name[0] == '.')
			continue;
		devices++;
		snprintf(path, sizeof(path), "sysfs/bus/pci/devices/%s/config",
			 e->d_name);
		CHECK((fd = open(path, O_RDONLY)) >= 0);
		CHECK(pread(fd, b, 64, 0) == 64 &&
		      pread(fd, line, 8, 256) == 0);
		close(fd);
		snprintf(path, sizeof(path), "sysfs/bus/pci/devices/%s/vendor",
			 e->d_name);
		CHECK(hexfile(path) ==
		      (unsigned char)b[0] + ((unsigned char)b[1] << 8));
		snprintf(path, sizeof(path), "sysfs/bus/pci/devices/%s/class",
			 e->d_name);
		klass = hexfile(path);
		CHECK(klass ==
		      ((unsigned char)b[9] | ((unsigned char)b[10] << 8) |
		       ((unsigned char)b[11] << 16)));
		snprintf(path, sizeof(path),
			 "sysfs/bus/pci/devices/%s/resource", e->d_name);
		CHECK((f = fopen(path, "r")));
		rows = 0;
		rom_start = rom_end = 0;
		while (fgets(line, sizeof(line), f)) {
			CHECK(sscanf(line, "%llx %llx %llx", &start, &end,
				     &flags) == 3);
			CHECK((!start && !end && !flags) || end >= start);
			if (rows == 6) {
				rom_start = start;
				rom_end = end;
			}
			rows++;
		}
		CHECK(rows == 7);
		fclose(f);
		if (klass >> 16 == 3) {
			displays++;
			snprintf(path, sizeof(path),
				 "sysfs/bus/pci/devices/%s/boot_vga",
				 e->d_name);
			CHECK(hexfile(path) <= 1);
			if (!geteuid() && rom_start && rom_end >= rom_start) {
				snprintf(path, sizeof(path),
					 "sysfs/bus/pci/devices/%s/config",
					 e->d_name);
				CHECK((fd = open(path, O_RDWR)) >= 0 &&
				      (mem = open("/dev/mem", O_RDONLY)) >= 0);
				CHECK(pread(fd, &saved, 4, 0x30) == 4);
				enabled = saved | 1;
				CHECK(pwrite(fd, &enabled, 4, 0x30) == 4);
				CHECK(pread(mem, b, 2, rom_start) == 2 &&
				      (unsigned char)b[0] == 0x55 &&
				      (unsigned char)b[1] == 0xaa);
				/* The x64 kernel can expose physical addresses above 4 GiB. */
				{
					off_t limit = lseek(mem, 0, SEEK_END);
					CHECK(limit >= (UINT64_C(1) << 32));
					CHECK(lseek(mem, 0, SEEK_SET) == 0);
					CHECK(pread(mem, b, 1, limit) == 0);
					CHECK(pread(mem, b, 1,
						    limit + rom_start) == 0);
				}
				CHECK(pread(mem, b, 1, -1) == -1 &&
				      errno == EINVAL);
				CHECK(lseek(mem, 0, SEEK_CUR) == 0 &&
				      lseek(mem, rom_start, SEEK_SET) ==
					      rom_start);
				CHECK(pwrite(fd, &saved, 4, 0x30) == 4);
				close(mem);
				close(fd);
			}
		}
	}
	closedir(dir);
	CHECK(devices && displays);
}
int main(void)
{
	int fd, dir;
	unsigned char leds[] = { 0xff, 0xa5, 0x5a, 0xc3 };
	char b[7], line[256], name[128];
	FILE *f;
	unsigned start, end, i;
	unsigned char ports[65536] = { 0 };
	alarm(20);
	CHECK(aux(11) == getuid() && aux(12) == geteuid() &&
	      aux(13) == getgid() && aux(14) == getegid());
	CHECK(aux(23) == (getuid() != geteuid() || getgid() != getegid()));
	CHECK((dir = open(".", O_RDONLY | O_DIRECTORY)) >= 0);
	CHECK((fd = open("target", O_CREAT | O_WRONLY, 0600)) >= 0);
	CHECK(write(fd, "payload", 7) == 7);
	close(fd);
	CHECK(!symlink("target", "link"));
	CHECK(!syscall(301, dir, "link", 0));
	CHECK((fd = open("target", O_RDONLY)) >= 0 && read(fd, b, 7) == 7 &&
	      !memcmp(b, "payload", 7));
	close(fd);
	CHECK(syscall(301, dir, "target", 0x4000) == -1 && errno == EINVAL &&
	      !access("target", F_OK));
	CHECK(!syscall(301, dir, "target", 0));
	CHECK(!mkdir("empty", 0700));
	CHECK(syscall(301, dir, "empty", 0) == -1 && errno == EISDIR);
	CHECK(!syscall(301, dir, "empty", 0x200));
	CHECK(syscall(301, -1, "missing", 0) == -1 && errno == EBADF);
	close(dir);
	sockets(0);
	sockets(1);
	fence();
	CHECK((fd = open("/dev/tty0", O_RDONLY | O_NONBLOCK)) >= 0);
	CHECK(!ioctl(fd, 0x4b31, leds));
	CHECK(leds[0] <= 7 && leds[1] == 0xa5 && leds[2] == 0x5a &&
	      leds[3] == 0xc3);
	close(fd);
	CHECK((f = fopen("/proc/ioports", "r")));
	while (fgets(line, sizeof(line), f)) {
		CHECK(strchr(line, '\n'));
		CHECK(sscanf(line, "%x-%x : %127[^\n]", &start, &end, name) ==
			      3 &&
		      start <= end && end <= 65535);
		if (strstr(name, "keyboard") || strstr(name, "timer"))
			for (i = start; i <= end; i++)
				ports[i] = 1;
	}
	fclose(f);
	CHECK(ports[0x40] && ports[0x41] && ports[0x42] && ports[0x43] &&
	      ports[0x60] && ports[0x64]);
	for (i = 0x3b0; i < 0x3e0; i++)
		CHECK(!ports[i]);
	pci();
	return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall -pthread "$probe_dir/probe.c" -o "$probe_dir/probe" -ldl
mkdir "$probe_dir/sysfs"
mount -t sysfs sysfs "$probe_dir/sysfs"
sysfs_mounted=1
(cd "$probe_dir"; exec "$probe_dir/probe" "$@")
