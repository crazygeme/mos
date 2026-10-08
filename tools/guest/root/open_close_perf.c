/*
 * Open/close syscall throughput benchmark for MOS, LFS, and Red Hat 9.
 * Build: gcc -O2 -Wall -W -o open_close_perf open_close_perf.c
 * Run:   ./open_close_perf                 (requires permission to mount tmpfs)
 *        ./open_close_perf --mount-dir /dev/shm
 *
 * Measures repeated SYS_open(path, O_RDONLY) + SYS_close(fd), with no I/O.
 * Normal and symlink paths refer to the same empty regular file. The mount
 * case opens an empty file on a temporary tmpfs mount, or in a private
 * directory on an existing mounted filesystem supplied with --mount-dir.
 * Different filesystem implementations affect the mount comparison.
 * All paths are absolute. Setup, warmup, printing, and cleanup are excluded.
 * These are warm-cache, single-process measurements. A pair is one successful
 * open and close; syscalls_per_s counts both, not independent syscall rates.
 * Time is checked every 256 pairs, so samples can exceed --seconds by a batch.
 * gettimeofday() supports older guests; do not change system time during a run.
 * Guest scheduling and syscall logging affect results.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(SYS_open) || !defined(SYS_close)
#error "This benchmark requires the open and close syscall numbers."
#endif

typedef unsigned long long count_t;
static char workspace[PATH_MAX], mount_dir[PATH_MAX];
static char normal_path[PATH_MAX], symlink_path[PATH_MAX], mount_path[PATH_MAX];
static int mounted, cleanup_failed;
static volatile sig_atomic_t stopped;

static void cleanup(void)
{
    if (symlink_path[0]) {
        if (unlink(symlink_path) < 0 && errno != ENOENT)
            goto failed;
        symlink_path[0] = '\0';
    }
    if (normal_path[0]) {
        if (unlink(normal_path) < 0 && errno != ENOENT)
            goto failed;
        normal_path[0] = '\0';
    }
    if (mount_path[0]) {
        if (unlink(mount_path) < 0 && errno != ENOENT)
            goto failed;
        mount_path[0] = '\0';
    }
    if (mounted) {
        if (umount(mount_dir) < 0)
            goto failed;
        mounted = 0;
    }
    if (mount_dir[0]) {
        if (rmdir(mount_dir) < 0 && errno != ENOENT)
            goto failed;
        mount_dir[0] = '\0';
    }
    if (workspace[0]) {
        if (rmdir(workspace) < 0 && errno != ENOENT)
            goto failed;
        workspace[0] = '\0';
    }
    return;
failed:
    perror("benchmark cleanup (temporary directories retained)");
    cleanup_failed = 1;
}

static void fail(const char *operation)
{
    perror(operation);
    exit(1);
}

static void on_signal(int sig)
{
    stopped = sig;
}

static void check_signal(void)
{
    if (stopped) {
        fprintf(stderr, "%s\n", stopped == SIGALRM ?
                "Benchmark sample timed out." : "Benchmark interrupted.");
        exit(128 + stopped);
    }
}

static void usage(const char *name)
{
    printf("Usage: %s [options]\n"
           "  --seconds N      Minimum seconds per sample, 1..3600 (2)\n"
           "  --repeats N      Samples per path, 1..100 (3)\n"
           "  --warmup N       Untimed open/close pairs, 0..1000000000 (1000)\n"
           "  --directory DIR  Parent of temporary normal/symlink files (/tmp)\n"
           "  --mount-dir DIR  Use a directory on an existing mounted filesystem\n"
           "                   instead of creating a tmpfs mount\n"
           "  --help           Display usage\n"
           "Default mount setup requires mount permission. --mount-dir creates\n"
           "only a private temporary directory and never unmounts that filesystem.\n"
           "Output is CSV. Each pair is one open plus one close; syscalls_per_s\n"
           "is twice pairs_per_s. Setup and warmup are excluded from timing.\n"
           "Each sample (including warmup) has a timeout of --seconds + 30.\n",
           name);
}

static count_t number(const char *text, count_t maximum)
{
    char *end;
    count_t value;
    errno = 0;
    if (!*text || strspn(text, "0123456789") != strlen(text))
        goto invalid;
    value = strtoull(text, &end, 10);
    if (errno || *end || value > maximum)
        goto invalid;
    return value;
invalid:
    fprintf(stderr, "Invalid decimal argument: %s\n", text);
    exit(2);
}

static void path_join(char *out, const char *parent, const char *name)
{
    int length = snprintf(out, PATH_MAX, "%s/%s", parent, name);
    if (length < 0 || length >= PATH_MAX) {
        out[0] = '\0';
        fputs("Temporary path is too long.\n", stderr);
        exit(1);
    }
}

static void temporary_dir(char *out, const char *parent)
{
    char absolute[PATH_MAX];
    if (!realpath(parent, absolute))
        fail("resolve temporary parent directory");
    path_join(out, absolute, "open-close-XXXXXX");
    if (!mkdtemp(out)) {
        out[0] = '\0';
        fail("mkdtemp");
    }
}

static void create_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        fail("create benchmark file");
    if (close(fd) < 0)
        fail("close benchmark file");
}

static void pair(const char *path)
{
    long fd = syscall(SYS_open, path, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "open path: %s\n", path);
        fail("SYS_open");
    }
    /* Never retry close: an interrupted close may already have released fd. */
    if (syscall(SYS_close, (int)fd) < 0)
        fail("SYS_close");
}

static void timestamp(struct timeval *time)
{
    if (gettimeofday(time, NULL) < 0)
        fail("gettimeofday");
}

static double interval(const struct timeval *start, const struct timeval *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_usec - start->tv_usec) / 1000000.0;
}

static void sample(const char *kind, const char *path, unsigned int repetition,
                   unsigned int duration, count_t warmup)
{
    struct timeval start, end, previous;
    count_t i, pairs = 0;
    double elapsed;
    unsigned int batch;

    check_signal();
    alarm(duration + 30);
    for (i = 0; i < warmup; ++i) {
        if (i % 256 == 0)
            check_signal();
        pair(path);
    }
    timestamp(&start);
    previous = start;
    do {
        check_signal();
        for (batch = 0; batch < 256; ++batch)
            pair(path);
        pairs += 256;
        timestamp(&end);
        if (interval(&previous, &end) < 0.0) {
            fputs("Clock moved backwards; sample is invalid.\n", stderr);
            exit(1);
        }
        previous = end;
        elapsed = interval(&start, &end);
    } while (elapsed < (double)duration);
    alarm(0);
    check_signal();
    printf("%s,%u,%llu,%.6f,%.3f,%.3f,%.3f\n", kind, repetition,
           pairs, elapsed, (double)pairs / elapsed,
           2.0 * (double)pairs / elapsed, elapsed * 1000000.0 / (double)pairs);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    unsigned int duration = 2, repeats = 3, repetition;
    count_t warmup = 1000;
    const char *parent = "/tmp", *existing_mount = NULL, *option, *value;
    int i;
    struct sigaction action;

    for (i = 1; i < argc; ++i) {
        option = argv[i];
        if (!strcmp(option, "--help") || !strcmp(option, "-h")) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 == argc) {
            fprintf(stderr, "Missing value for option: %s\n", option);
            return 2;
        }
        value = argv[++i];
        if (!strcmp(option, "--seconds"))
            duration = (unsigned int)number(value, 3600);
        else if (!strcmp(option, "--repeats"))
            repeats = (unsigned int)number(value, 100);
        else if (!strcmp(option, "--warmup"))
            warmup = number(value, 1000000000);
        else if (!strcmp(option, "--directory"))
            parent = value;
        else if (!strcmp(option, "--mount-dir"))
            existing_mount = value;
        else {
            fprintf(stderr, "Unknown option: %s\n", option);
            return 2;
        }
    }
    if (!duration || !repeats) {
        fputs("--seconds and --repeats must be positive.\n", stderr);
        return 2;
    }
    if (atexit(cleanup) != 0) {
        fputs("Cannot register cleanup.\n", stderr);
        return 1;
    }
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_signal;
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGALRM, &action, NULL) < 0)
        fail("sigaction");

    temporary_dir(workspace, parent);
    path_join(normal_path, workspace, "file");
    path_join(symlink_path, workspace, "link");
    create_file(normal_path);
    if (symlink("file", symlink_path) < 0)
        fail("symlink");
    if (existing_mount) {
        temporary_dir(mount_dir, existing_mount);
    } else {
        path_join(mount_dir, workspace, "mount");
        if (mkdir(mount_dir, 0700) < 0)
            fail("mkdir mount point");
        if (mount("none", mount_dir, "tmpfs", 0, NULL) < 0) {
            fputs("Cannot mount tmpfs; use --mount-dir with an existing mounted\n"
                  "filesystem directory (for example /dev/shm).\n", stderr);
            fail("mount tmpfs");
        }
        mounted = 1;
    }
    path_join(mount_path, mount_dir, "file");
    create_file(mount_path);
    fprintf(stderr, "normal: %s\nsymlink: %s\nmount: %s (%s)\n",
            normal_path, symlink_path, mount_path,
            existing_mount ? "user-supplied filesystem" : "temporary tmpfs");
    puts("path_kind,repeat,pairs,seconds,pairs_per_s,syscalls_per_s,us_per_pair");
    for (repetition = 1; repetition <= repeats; ++repetition) {
        sample("normal", normal_path, repetition, duration, warmup);
        sample("symlink", symlink_path, repetition, duration, warmup);
        sample("mount", mount_path, repetition, duration, warmup);
    }
    cleanup();
    return cleanup_failed ? 1 : 0;
}
