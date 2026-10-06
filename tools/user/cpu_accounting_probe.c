/* Guest CPU accounting regression. Build with gcc -O2 -std=gnu99 -pthread. */
#define _GNU_SOURCE
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

static int failures;
static void check(int ok, const char *name)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}
static double wall(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec / 1000000.0;
}
static double value(struct timeval t) { return t.tv_sec + t.tv_usec / 1000000.0; }
static double total(struct rusage *r) { return value(r->ru_utime) + value(r->ru_stime); }
static void burn(double seconds, int kernel)
{
    double end = wall() + seconds;
    volatile unsigned x = 1;
    do {
        for (unsigned i = 0; i < 10000; i++) {
            if (kernel) (void)syscall(20);
            else x = x * 1664525U + 1013904223U;
        }
    } while (wall() < end);
}
static void *worker(void *unused) { (void)unused; burn(.35, 0); return NULL; }
static void cpu_stat(unsigned long long v[3])
{
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long nice;
    if (!f || fscanf(f, "cpu %llu %llu %llu %llu", &v[0], &nice, &v[1], &v[2]) != 4) {
        perror("/proc/stat"); exit(2);
    }
    fclose(f);
}
int main(void)
{
    struct rusage a, b, ca, cb, child;
    struct tms t;
    struct timespec clock, res;
    long hz = sysconf(_SC_CLK_TCK);
    getrusage(RUSAGE_SELF, &a);
    burn(.35, 0);
    getrusage(RUSAGE_SELF, &b);
    check(value(b.ru_utime) - value(a.ru_utime) > .15, "user execution accrues user CPU time");
    a = b;
    burn(.35, 1);
    getrusage(RUSAGE_SELF, &b);
    check(value(b.ru_stime) - value(a.ru_stime) > .05, "syscall execution accrues system CPU time");
    a = b;
    usleep(400000);
    getrusage(RUSAGE_SELF, &b);
    check(total(&b) - total(&a) < .08, "sleep excludes off-CPU time");
    times(&t);
    check(labs(t.tms_utime - (long)(value(b.ru_utime) * hz + .5)) <= 3 &&
          labs(t.tms_stime - (long)(value(b.ru_stime) * hz + .5)) <= 3,
          "times and getrusage agree");
    check(syscall(265, 2, &clock) == 0 &&
          clock.tv_sec + clock.tv_nsec / 1e9 >= total(&b) - .03,
          "process CPU clock reports CPU time");
    check(syscall(266, 2, &res) == 0 && res.tv_sec == 0 && res.tv_nsec == 10000000,
          "CPU clock advertises timer resolution");
    errno = 0;
    check(getrusage(12345, &b) == -1 && errno == EINVAL, "invalid usage selector returns EINVAL");

    getrusage(RUSAGE_SELF, &a);
    getrusage(RUSAGE_CHILDREN, &ca);
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, worker, NULL);
    if (rc) { fprintf(stderr, "pthread_create: %d\n", rc); return 2; }
    pthread_join(thread, NULL);
    getrusage(RUSAGE_SELF, &b);
    getrusage(RUSAGE_CHILDREN, &cb);
    check(total(&b) - total(&a) > .15, "exited thread remains in process CPU totals");
    check(total(&cb) == total(&ca), "thread exit does not accrue child CPU time");

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (!pid) {
        getrusage(RUSAGE_SELF, &a);
        if (total(&a) > .05) _exit(7);
        burn(.15, 0);
        _exit(0);
    }
    usleep(650000);
    unsigned long long before[3], after[3];
    cpu_stat(before);
    int status;
    if (wait4(pid, &status, 0, &child) != pid) { perror("wait4"); return 2; }
    cpu_stat(after);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork resets process CPU totals");
    check(total(&child) > .03 && total(&child) < .35, "delayed reaping excludes zombie wall time");
    check(after[0] >= before[0] && after[1] >= before[1] && after[2] >= before[2],
          "system CPU counters survive process reaping");
    getrusage(RUSAGE_CHILDREN, &cb);
    times(&t);
    check(labs(t.tms_cutime - (long)(value(cb.ru_utime) * hz + .5)) <= 3 &&
          labs(t.tms_cstime - (long)(value(cb.ru_stime) * hz + .5)) <= 3,
          "times and child usage agree");

    pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (!pid) {
        pid_t grandchild = fork();
        if (grandchild < 0) _exit(8);
        if (!grandchild) { burn(.15, 0); _exit(0); }
        if (waitpid(grandchild, &status, 0) != grandchild) _exit(9);
        burn(.15, 0);
        _exit(0);
    }
    if (wait4(pid, &status, 0, &child) != pid) { perror("wait4 descendants"); return 2; }
    check(total(&child) > .18, "wait4 includes waited-for descendant CPU time");
    printf("CPU accounting: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
