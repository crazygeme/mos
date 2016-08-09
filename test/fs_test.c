#include <namespace.h>
#ifdef WIN32
#include <windows.h>
#include <osdep.h>
#elif MACOS
#include <osdep.h>
#else
#include <ps.h>
#include <lock.h>
#include <config.h>
#include <klib.h>
#include <mount.h>
#include <timer.h>
#include <errno.h>
#include <cyclebuf.h>
#endif
#include <unistd.h>
#include <cache.h>

static void list_dir(char* name, int depth)
{
    char file[32];
    unsigned mode;
    DIR dir = fs_opendir(name);
    while (fs_readdir(dir, file, &mode))
    {
        int i = 0;
        if (!strcmp(file, ".") || !strcmp(file, ".."))
            continue;

        for (i = 0; i < depth; i++)
            printf("\t");
        printf("%s\n", file);
        if (S_ISDIR(mode))
        {
            char path[32];
            strcpy(path, name);
            if (strcmp(name, "/"))
                strcat(path, "/");
            strcat(path, file);
            list_dir(path, depth + 1);
        }
    }
    fs_closedir(dir);
}

static void test_stat(char* path)
{
    struct stat s;
    fs_stat(path, &s);
    printk("%s: is dir %d, size %d\n", path, S_ISDIR(s.st_mode), s.st_size);
}

static unsigned _time_now()
{
    time_t t;
    timer_current(&t);
    return (t.seconds * 1000 + t.milliseconds);
}
static void test_write()
{
    int fd = fs_open("/lib/libc.so.6");
    char* buf = 0;
    int i = 0;
    time_t now;
    unsigned t;
    timer_current(&now);
    printk("%d: write begin, fd %d\n", now.seconds * 60 + now.milliseconds, fd);
    if (fd == -1)
        return;

    buf = kmalloc(1024);
    memset(buf, 'd', 1024);
    t = _time_now();
    for (i = 0; i < (3 * 1024); i++)
    {
        if (i % 100 == 0)
        {
            unsigned span = _time_now() - t;
            unsigned speed = 0;
            if (span)
            {
                speed = ((100 * 1024) / span) * 1000;
            }
            t = t + span;
            printk("write index %d, speed %h/s\n", i, speed);
            if (TestControl.test_ffs)
            {
                extern void report_time();
                extern void report_hdd_time();
                extern void report_cache();
                extern void report_sched_time();
                extern void mm_report();
                report_time();
                report_hdd_time();
                report_cache();
                report_sched_time();
                mm_report();
            }
        }
        fs_write(fd, i * 1024, buf, 1024);

    }
    kfree(buf);
    fs_close(fd);

    timer_current(&now);
    printk("%d: write end\n", now.seconds * 60 + now.milliseconds);
}

static void test_read()
{
    int fd = fs_open("/lib/libc.so.6");
    char* buf = 0;
    int i = 0;
    int count = 1;
    time_t now;
    unsigned t;
    timer_current(&now);
    printk("%d: read  begin\n", now.seconds * 60 + now.milliseconds);
    if (fd == -1)
        return;

    buf = kmalloc(1024);
    memset(buf, 0, 1024);
    t = _time_now();
    while (1)
    {
        if ((count % 1000) == 0)
        {
            unsigned span = _time_now() - t;
            unsigned speed = 0;

            if (span)
            {
                speed = ((1024 * 4096) / span) * 1000;
            }
            //t = t + span;

            printk("read count %d, speed %h/s\n", count, speed);
            if (TestControl.test_ffs)
            {
                extern void report_time();
                extern void report_hdd_time();
                extern void report_cache();
                extern void report_sched_time();
                extern void mm_report();
                report_time();
                report_hdd_time();
                report_cache();
                report_sched_time();
                mm_report();
            }
            t = _time_now();

        }
        for (i = 0; i < 4; i++)
        {
            //memset(buf, 0, 1024);
            fs_read(fd, i * 1024, buf, 1024);

        }
        count++;
    }
    kfree(buf);
    fs_close(fd);

    timer_current(&now);
    printk("%d: read end\n", now.seconds * 60 + now.milliseconds);
}

void test_ns()
{
    unsigned fd;
    char text[32];
    klogquota();

    printk("test_ns\n");
    list_dir("/", 0);

    if (TestControl.test_fs_read)
        test_read();

    if (TestControl.test_fs_write)
        test_write();

    klogquota();

    for (;;)
    {
        __asm__("hlt");
    }
}

