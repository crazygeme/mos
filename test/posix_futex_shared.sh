#!/bin/sh
set -eu
BASE=/root/tests/posix_futex_shared
trap 'rm -rf "$BASE"; rm -f /dev/shm/mos-futex-shared-test' EXIT
mkdir -p "$BASE"
cat > "$BASE/test.c" <<'SOURCE'
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PATH "/dev/shm/mos-futex-shared-test"

static void fail(const char *msg)
{
	perror(msg);
	exit(1);
}

static int wake(int *word, int flags)
{
	return syscall(SYS_futex, word, FUTEX_WAKE | flags, 1, NULL, NULL, 0);
}

static void check(int file_backed, int private)
{
	long size = sysconf(_SC_PAGESIZE);
	int fd = -1, channel[2], status, attempts, n;
	int *mapping;
	pid_t child;
	char ready;
	struct timespec pause = { 0, 1000000 };

	if (file_backed) {
		fd = open(PATH, O_CREAT | O_EXCL | O_RDWR, 0600);
		if (fd < 0 || ftruncate(fd, size) != 0)
			fail("create shared file");
	}
	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE,
		       MAP_SHARED | (file_backed ? 0 : MAP_ANONYMOUS), fd, 0);
	if (mapping == MAP_FAILED || pipe(channel) != 0)
		fail("map shared words");
	mapping[0] = mapping[1] = 0;
	child = fork();
	if (child < 0)
		fail("fork");
	if (!child) {
		int *word = mapping;
		struct timespec timeout = { private ? 0 : 3, private ? 200000000 : 0 };
		int result;
		close(channel[0]);
		if (file_backed) {
			int other = open(PATH, O_RDWR);
			if (other < 0)
				fail("reopen shared file");
			word = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, other, 0);
			if (word == MAP_FAILED || word == mapping)
				fail("map shared file at a distinct address");
			close(other);
			munmap(mapping, size);
			close(fd);
		}
		if (write(channel[1], "R", 1) != 1)
			fail("signal waiter readiness");
		result = syscall(SYS_futex, word, FUTEX_WAIT |
				 (private ? FUTEX_PRIVATE_FLAG : 0), 0, &timeout, NULL, 0);
		_exit(private ? !(result == -1 && errno == ETIMEDOUT) : result != 0);
	}
	close(channel[1]);
	if (read(channel[0], &ready, 1) != 1)
		fail("read waiter readiness");
	close(channel[0]);
	if (file_backed && unlink(PATH) != 0)
		fail("unlink mapped shared file");
	for (attempts = 0; attempts < 1000; attempts++) {
		if (wake(mapping + 1, 0) != 0 || wake(mapping, FUTEX_PRIVATE_FLAG) != 0)
			fail("unrelated futex woke a waiter");
		n = wake(mapping, 0);
		if (n < 0 || (private && n != 0))
			fail("shared wake result");
		if (n == 1)
			break;
		nanosleep(&pause, NULL);
	}
	if (!private && attempts == 1000)
		fail("shared wake did not find waiter");
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
		fail("waiter result");
	munmap(mapping, size);
	if (fd >= 0)
		close(fd);
}

int main(void)
{
	alarm(15);
	check(1, 0);
	check(0, 0);
	check(1, 1);
	return 0;
}
SOURCE
gcc -Wall -Wextra -o "$BASE/test" "$BASE/test.c"
"$BASE/test"
