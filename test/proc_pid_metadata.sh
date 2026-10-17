#!/bin/sh
# Validate process and thread metadata through procfs and procps.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc-metadata.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.c" <<'C'
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { \
 fprintf(stderr, "%d: %s (errno=%d)\n", __LINE__, #x, errno); exit(1); \
} } while (0)
static int ready[2], finish[2], child_tid;
static int worker(void *unused)
{
 char c = 'R';
 (void)unused;
 CHECK(write(ready[1], &c, 1) == 1);
 CHECK(read(finish[0], &c, 1) == 1);
 return 0;
}
static size_t contents(int dir, const char *name, char *buf, size_t size)
{
 int fd = openat(dir, name, O_RDONLY);
 size_t total = 0;
 ssize_t n;
 CHECK(fd >= 0);
 while ((n = read(fd, buf + total, size - total - 1)) > 0) {
  total += n;
  CHECK(total < size - 1);
 }
 CHECK(n == 0 && total > 0);
 buf[total] = 0;
 CHECK(lseek(fd, 0, SEEK_SET) == 0);
 CHECK(read(fd, buf, 1) == 1);
 close(fd);
 return total;
}
static void metadata(const char *path, int pid, int tid)
{
 char buf[131072], *line, link[4096];
 int dir = open(path, O_RDONLY | O_DIRECTORY), got_pid = 0, got_tgid = 0;
 struct stat st;
 CHECK(dir >= 0);
 contents(dir, "status", buf, sizeof(buf));
 for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
  char *colon = strchr(line, ':');
  CHECK(colon && colon[1] == '\t');
  if (!strncmp(line, "Pid:", 4)) got_pid = atoi(colon + 2);
  if (!strncmp(line, "Tgid:", 5)) got_tgid = atoi(colon + 2);
 }
 CHECK(got_pid == tid && got_tgid == pid);
 contents(dir, "stat", buf, sizeof(buf));
 CHECK(atoi(buf) == tid && strchr(buf, '(') && strchr(buf, ')'));
 contents(dir, "statm", buf, sizeof(buf));
 CHECK(strtoull(buf, NULL, 10) > 0);
 contents(dir, "maps", buf, sizeof(buf));
 CHECK(strstr(buf, "r-xp") && strstr(buf, "/probe"));
 contents(dir, "comm", buf, sizeof(buf));
 CHECK(strchr(buf, '\n'));
 contents(dir, "cmdline", buf, sizeof(buf));
 CHECK(strstr(buf, "/probe"));
 contents(dir, "environ", buf, sizeof(buf));
 CHECK(readlinkat(dir, "cwd", link, sizeof(link)) > 0);
 CHECK(readlinkat(dir, "exe", link, sizeof(link)) > 0);
 CHECK(fstatat(dir, "fd", &st, 0) == 0 && S_ISDIR(st.st_mode));
 CHECK(readlinkat(dir, "fd/0", link, sizeof(link)) > 0);
 if (tid != pid) {
  CHECK(fstatat(dir, "task", &st, 0) == -1 && errno == ENOENT);
 }
 close(dir);
}
int main(void)
{
 char path[128], c, line[128];
 int pid = getpid(), tid, parsed_pid, parsed_tid, seen = 0, status;
 void *stack;
 FILE *ps;
 alarm(30);
 CHECK(pipe(ready) == 0 && pipe(finish) == 0);
 stack = mmap(NULL, 1024 * 1024, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
 CHECK(stack != MAP_FAILED);
 tid = clone(worker, (char *)stack + 1024 * 1024,
             CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
             CLONE_THREAD | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID,
             NULL, &child_tid, NULL, &child_tid);
 CHECK(tid > 0 && read(ready[0], &c, 1) == 1);
 snprintf(path, sizeof(path), "/proc/%d", pid);
 metadata(path, pid, pid);
 metadata("/proc/self", pid, pid);
 snprintf(path, sizeof(path), "/proc/%d/task/%d", pid, pid);
 metadata(path, pid, pid);
 snprintf(path, sizeof(path), "/proc/%d/task/%d", pid, tid);
 metadata(path, pid, tid);
 snprintf(path, sizeof(path), "ps -L -p %d -o pid=,lwp=", pid);
 CHECK((ps = popen(path, "r")));
 while (fgets(line, sizeof(line), ps)) {
  CHECK(sscanf(line, "%d %d", &parsed_pid, &parsed_tid) == 2);
  CHECK(parsed_pid == pid && (parsed_tid == pid || parsed_tid == tid));
  seen |= parsed_tid == pid ? 1 : 2;
 }
 status = pclose(ps);
 CHECK(status == 0 && seen == 3);
 CHECK(write(finish[1], "Q", 1) == 1);
 while (__atomic_load_n(&child_tid, __ATOMIC_ACQUIRE)) {
  int value = __atomic_load_n(&child_tid, __ATOMIC_RELAXED);
  if (value) syscall(SYS_futex, &child_tid, FUTEX_WAIT, value, NULL, NULL, 0);
 }
 snprintf(path, sizeof(path), "/proc/%d/task/%d/status", pid, tid);
 CHECK(access(path, F_OK) == -1 && errno == ENOENT);
 CHECK(munmap(stack, 1024 * 1024) == 0);
 puts("proc_pid_metadata: PASS");
 return 0;
}
C
"${CC:-gcc}" -std=gnu11 -O2 -Wall "$probe_dir/probe.c" -o "$probe_dir/probe"
"$probe_dir/probe"
