#!/bin/sh
# Validate process lookup lifetime during concurrent exit and reaping.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc-exit.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
cat > "$probe_dir/probe.c" <<'MOS_GUEST_C'
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static struct {
    volatile int selected, finished;
    unsigned reads[4];
} *shared;
static void publish(volatile int *target, int value)
{
    __asm__ __volatile__("xchgl %0, %1"
                         : "+r"(value), "+m"(*target) : : "memory");
}
static void reader(int index)
{
    char path[100], text[4096];
    while (!shared->finished) {
        int pid = shared->selected;
        if (!pid) continue;
        for (int thread_path = 0; thread_path < 2; thread_path++) {
            if (thread_path)
                snprintf(path, sizeof(path), "/proc/%d/task/%d/status", pid, pid);
            else
                snprintf(path, sizeof(path), "/proc/%d/status", pid);
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            ssize_t n = read(fd, text, sizeof(text));
            assert(n >= 0 || errno == ESRCH || errno == ENOENT);
            if (n > 0) shared->reads[index]++;
            close(fd);
        }
    }
}
int main(void)
{
    alarm(120);
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(shared != MAP_FAILED);
    shared->selected = 0;
    shared->finished = 0;
    for (int i = 0; i < 4; i++) shared->reads[i] = 0;
    pid_t readers[4];
    for (int i = 0; i < 4; i++) {
        readers[i] = fork();
        assert(readers[i] >= 0);
        if (!readers[i]) { reader(i); _exit(0); }
    }
    for (int i = 0; i < 500; i++) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) { usleep(1000); _exit(0); }
        publish(&shared->selected, pid);
        int status;
        assert(waitpid(pid, &status, 0) == pid && status == 0);
    }
    publish(&shared->finished, 1);
    for (int i = 0; i < 4; i++) {
        int status;
        assert(waitpid(readers[i], &status, 0) == readers[i] && status == 0);
    }
    unsigned reads = 0;
    for (int i = 0; i < 4; i++) reads += shared->reads[i];
    assert(reads > 0);
    printf("procfs exit lookup: PASS (%u reads)\n", reads);
    assert(munmap(shared, sizeof(*shared)) == 0);
    return 0;
}
MOS_GUEST_C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
