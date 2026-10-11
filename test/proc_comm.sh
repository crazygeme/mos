#!/bin/sh
# Validate command-name snapshots and process/thread directory entries.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc-comm.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void check_directory(const char *path)
{
	DIR *directory = opendir(path);
	struct dirent *entry;
	int found = 0;
	assert(directory);
	while ((entry = readdir(directory)))
		if (!strcmp(entry->d_name, "comm"))
			found = 1;
	assert(closedir(directory) == 0);
	assert(found);
}

static void check_file(const char *path, const char *expected)
{
	int fd = open(path, O_RDONLY);
	char buffer[32];
	struct stat info;
	size_t length = strlen(expected);
	assert(fd >= 0);
	assert(fstat(fd, &info) == 0);
	assert(S_ISREG(info.st_mode));
	assert((size_t)info.st_size == length);
	for (size_t index = 0; index < length; index++) {
		assert(read(fd, buffer, 1) == 1);
		assert(buffer[0] == expected[index]);
	}
	assert(read(fd, buffer, 1) == 0);
	assert(lseek(fd, 0, SEEK_SET) == 0);
	assert(read(fd, buffer, sizeof(buffer)) == (ssize_t)length);
	assert(memcmp(buffer, expected, length) == 0);
	assert(close(fd) == 0);
}

int main(int argc, char **argv)
{
	char expected[17];
	char path[128];
	const char *base = strrchr(argv[0], '/');
	pid_t child;
	int status;
	base = base ? base + 1 : argv[0];
	snprintf(expected, sizeof(expected), "%.15s\n", base);
	check_file("/proc/self/comm", expected);
	snprintf(path, sizeof(path), "/proc/%ld/comm", (long)getpid());
	check_file(path, expected);
	snprintf(path, sizeof(path), "/proc/%ld/task/%ld/comm",
		 (long)getpid(), (long)getpid());
	check_file(path, expected);
	check_directory("/proc/self");
	snprintf(path, sizeof(path), "/proc/%ld/task/%ld",
		 (long)getpid(), (long)getpid());
	check_directory(path);
	if (argc == 1) {
		child = fork();
		assert(child >= 0);
		if (!child) {
			check_file("/proc/self/comm", expected);
			_exit(0);
		}
		assert(waitpid(child, &status, 0) == child);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	puts("proc comm checks passed");
	return 0;
}
MOS_GUEST_C
gcc -std=gnu99 -O2 -Wall -Werror "$probe_dir/probe.c" -o "$probe_dir/comm-probe"
"$probe_dir/comm-probe"
cp "$probe_dir/comm-probe" "$probe_dir/command-name-exceeding-fifteen-bytes"
"$probe_dir/command-name-exceeding-fifteen-bytes" long
