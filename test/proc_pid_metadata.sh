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
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { \
 fprintf(stderr, "%d: %s (errno=%d)\n", __LINE__, #x, errno); exit(1); \
} } while (0)
/* RH9 libc predates the *at wrappers; exercise the kernel ABI directly. */
#ifdef __i386__
#define openat(d, p, f) syscall(295, (d), (p), (f), 0)
#define readlinkat(d, p, b, n) syscall(305, (d), (p), (b), (n))
#define fstatat(d, p, s, f) syscall(300, (d), (p), (s), (f))
#define stat_at stat64
#else
#define stat_at stat
#endif
static int ready[2], finish[2];
static void *worker(void *unused)
{
 char c;
 int tid = syscall(SYS_gettid);
 (void)unused;
 CHECK(write(ready[1], &tid, sizeof(tid)) == sizeof(tid));
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
 struct stat_at st;
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
int main(int argc, char **argv)
{
 char path[128], line[128];
 int process_only = argc > 1;
 (void)argv;
 int pid = getpid(), tid, parsed_pid, parsed_tid, seen = 0, status;
 pthread_t thread;
 FILE *ps;
 alarm(30);
 CHECK(pipe(ready) == 0 && pipe(finish) == 0);
 CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
 CHECK(read(ready[0], &tid, sizeof(tid)) == sizeof(tid) && tid > 0);
 snprintf(path, sizeof(path), "/proc/%d", pid);
 metadata(path, pid, pid);
 metadata("/proc/self", pid, pid);
 snprintf(path, sizeof(path), "/proc/%d/task/%d", pid, pid);
 metadata(path, pid, pid);
 snprintf(path, sizeof(path), "/proc/%d/task/%d", pid, tid);
 metadata(path, pid, tid);
 if (process_only)
  snprintf(path, sizeof(path), "ps -p %d -o pid=", pid);
 else
  snprintf(path, sizeof(path), "ps -L -p %d -o pid=,lwp=", pid);
 CHECK((ps = popen(path, "r")));
 while (fgets(line, sizeof(line), ps)) {
  if (process_only) {
   CHECK(sscanf(line, "%d", &parsed_pid) == 1 && parsed_pid == pid);
   seen |= 1;
  } else {
   CHECK(sscanf(line, "%d %d", &parsed_pid, &parsed_tid) == 2);
   CHECK(parsed_pid == pid && (parsed_tid == pid || parsed_tid == tid));
   seen |= parsed_tid == pid ? 1 : 2;
  }
 }
 status = pclose(ps);
 CHECK(status == 0 && seen == (process_only ? 1 : 3));
 CHECK(write(finish[1], "Q", 1) == 1);
 CHECK(pthread_join(thread, NULL) == 0);
 snprintf(path, sizeof(path), "/proc/%d/task/%d/status", pid, tid);
 CHECK(access(path, F_OK) == -1 && errno == ENOENT);
 puts("proc_pid_metadata: PASS");
 return 0;
}
C
"${CC:-gcc}" -std=gnu99 -O2 -Wall "$probe_dir/probe.c" -pthread -o "$probe_dir/probe"
# RH9 procps lacks both -L and the lwp format; procfs checks above still
# validate both live threads and removal of the worker after pthread_join.
if ps -L -p $$ -o pid=,lwp= >/dev/null 2>&1; then
 "$probe_dir/probe"
else
 "$probe_dir/probe" --process-only
fi
