#!/bin/sh
set -eu

base=$(mktemp -d /tmp/file-fault-around.XXXXXX)
trap 'rm -rf "$base"' EXIT HUP INT TERM
cat > "$base/probe.c" <<'EOF'
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGES 16
#define PAGE 4096
static unsigned char *private_map;

static void check(int ok, const char *operation)
{
    if (!ok) {
        fprintf(stderr, "file_fault_around: %s failed\n", operation);
        exit(1);
    }
}

static void wait_ok(pid_t child)
{
    int status;
    check(child > 0 && waitpid(child, &status, 0) == child && status == 0,
          "child completion");
}

static void expect_fault(volatile unsigned char *address)
{
    int status;
    pid_t child = fork();
    if (child == 0) {
        unsigned char value = *address;
        _exit(value == 255 ? 2 : 3);
    }
    check(child > 0 && waitpid(child, &status, 0) == child &&
          WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "protected mapping");
}

static void *reader(void *arg)
{
    unsigned index = (unsigned long)arg;
    unsigned i, round;
    for (round = 0; round < 100; ++round)
        for (i = 0; i < PAGES; ++i)
            check(private_map[i * PAGE + 16] == (unsigned char)(i + 16),
                  "concurrent page contents");
    private_map[(8 + index) * PAGE] = 0x70 + index;
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char data[PAGES * PAGE], actual[PAGES * PAGE];
    unsigned char *observer, *shared, *offset, *guard, *protected;
    pthread_t threads[4];
    unsigned i, j;
    int fd;
    pid_t child;
    check(argc == 2, "file argument");
    fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open");
    for (i = 0; i < PAGES; ++i)
        for (j = 0; j < PAGE; ++j)
            data[i * PAGE + j] = i + j;
    check(write(fd, data, sizeof(data)) == sizeof(data), "file initialization");
    check(lseek(fd, 0, SEEK_SET) == 0 &&
          read(fd, actual, sizeof(actual)) == sizeof(actual), "cache population");

    private_map = mmap(0, sizeof(data), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    observer = mmap(0, sizeof(data), PROT_READ, MAP_PRIVATE, fd, 0);
    check(private_map != MAP_FAILED && observer != MAP_FAILED, "private mmap");
    private_map[3 * PAGE] = 0xd3;
    check(private_map[0] == 0 && private_map[3 * PAGE] == 0xd3 &&
          observer[3 * PAGE] == 3, "existing private copy");
    child = fork();
    if (child == 0) {
        private_map[4 * PAGE] = 0xe4;
        _exit(private_map[4 * PAGE] != 0xe4);
    }
    wait_ok(child);
    check(private_map[4 * PAGE] == 4, "fork isolation");
    for (i = 0; i < 4; ++i)
        check(pthread_create(&threads[i], 0, reader, (void *)(unsigned long)i) == 0,
              "thread creation");
    for (i = 0; i < 4; ++i) {
        check(pthread_join(threads[i], 0) == 0, "thread completion");
        check(private_map[(8 + i) * PAGE] == 0x70 + i &&
              observer[(8 + i) * PAGE] == 8 + i, "thread private writes");
    }

    offset = mmap(0, 4 * PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
    check(offset != MAP_FAILED, "offset mmap");
    for (i = 0; i < 4; ++i)
        check(offset[i * PAGE] == i + 1, "mapping offsets");

    protected = mmap(0, sizeof(data), PROT_READ, MAP_PRIVATE, fd, 0);
    check(protected != MAP_FAILED &&
          mprotect(protected + 2 * PAGE, PAGE, PROT_NONE) == 0, "mprotect");
    check(protected[PAGE] == 1, "page beside protected mapping");
    expect_fault(protected + 2 * PAGE);

    guard = mmap(0, (PAGES + 2) * PAGE, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(guard != MAP_FAILED, "guard reservation");
    check(mmap(guard + PAGE, sizeof(data), PROT_READ,
               MAP_PRIVATE | MAP_FIXED, fd, 0) == guard + PAGE, "guarded mmap");
    check(guard[PAGE] == 0 && guard[PAGES * PAGE] == PAGES - 1,
          "guarded page contents");
    expect_fault(guard);
    expect_fault(guard + (PAGES + 1) * PAGE);

    shared = mmap(0, sizeof(data), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(shared != MAP_FAILED && shared[0] == 0, "shared mmap");
    shared[5 * PAGE] = 0xa5;
    check(observer[5 * PAGE] == 0xa5 && private_map[5 * PAGE] == 0xa5,
          "shared visibility");
    check(munmap(shared, sizeof(data)) == 0, "shared writeback");
    check(lseek(fd, 5 * PAGE, SEEK_SET) == 5 * PAGE &&
          read(fd, actual, 1) == 1 && actual[0] == 0xa5, "descriptor contents");
    close(fd);
    puts("file_fault_around: PASS");
    return 0;
}
EOF
gcc -m32 -O2 -Wall -Werror -pthread "$base/probe.c" -o "$base/probe"
"$base/probe" "$base/data"
