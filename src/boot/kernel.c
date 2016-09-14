#include <klib.h>
#include <int.h>
#include <keyboard.h>
#include <mm.h>
#include <multiboot.h>
#include <dsr.h>
#include <timer.h>
#include <ps.h>
#include <block.h>
#include <hdd.h>
#include <vfs.h>
#include <mount.h>
#include <syscall.h>
#include <pagefault.h>
#include <console.h>
#include <kbchar.h>
#include <null.h>
#include <cache.h>
#include <pipechar.h>
#include <serial.h>
#include <vga.h>
#include <tests.h>

static void run(void);
_START static void init(multiboot_info_t* mb);
_STARTDATA static multiboot_info_t* _g_mb;
static multiboot_info_t* g_mb;
TEST_CONTROL TestControl;

_START void kmain(multiboot_info_t* mb, unsigned int magic)
{
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
    {
        return;
    }

    _g_mb = mb;

    init(mb);


    // never to here

    return;
}


static void kmain_process(void* param);
static void idle_process(void* param);
static void parse_kernel_cmdline();

void kmain_startup()
{
    int i = 0;

    fb_enable();

    g_mb = (multiboot_info_t*)((unsigned)_g_mb + KERNEL_OFFSET);
    // after klib_init, kmalloc/kfree/prink/etc are workable    
    klib_init();

    printk("parse kernel command line\n");
    parse_kernel_cmdline();

    printk("Init process\n");
    ps_init();

    printk("Init dsr\n");
    dsr_init();

    printk("Enable interrupts\n");
    int_enable_all();

    mm_del_user_map();

    printk("Init serial\n");
    serial_init_queue();

    printk("Init keyboard\n");
    kb_init();

    printk("Init timer\n");
    timer_init();


    printk("Caculate CPU caps\n");
    timer_calculate_cpu_cycle();


    if (TestControl.test_mm)
    {
        memcpy_measure();
        mm_test();
        malloc_test();
    }

    printk("Init page fault\n");
    pf_init();

#ifdef TEST_PS
    //ps_mmm();
    extern void test_ps_process();
    test_ps_process();
#endif

#ifdef TEST_LOCK
    extern void test_event_process();
    test_event_process();
#endif

#ifndef TEST_PS
#ifndef TEST_LOCK

    printk("Start first process\n");

    // create idle process
    ps_create(idle_process, 0, 1, ps_kernel);

    // create first process
    ps_create(kmain_process, 0, 3, ps_kernel);

    ps_create(dsr_process, 0, 4, ps_dsr);

    ps_kickoff();

    // never going to here because ps0 never exit
#endif
#endif


    run();

}

static void idle_process(void* param)
{
    task_struct* cur = CURRENT_TASK();
    while (1)
    {
        __asm__("hlt");

        task_sched();
    }
}

static void kmain_process(void* param)
{
    klog_init();

    if (TestControl.test_mmap)
    {
        vm_test();
        if (!TestControl.test_all)
            for (;;);
    }

    printk("Init vfs\n");
    vfs_init();

    printk("Init mount points\n");
    mount_init();

    printk("Init block devices\n");
    block_init();

    hdd_init();

    printk("Mount root fs to \"/\" \n");
    vfs_trying_to_mount_root();

    printk("Init char devices\n");
    chardev_init();

    kbchar_init();

    console_init();

    null_init();

    pipe_init();

    file_cache_init();

#ifdef TEST_BLOCK
    extern void test_block_process();
    test_block_process();
#endif

#ifdef TEST_MOUNT
    extern void dump_mount_point();
    dump_mount_point();
#endif

    if (TestControl.test_fs_read || TestControl.test_fs_write)
    {
        extern void test_ns();
        test_ns();
    }


    printk("Init system call table\n");
    syscall_init();

    klib_clear();

#ifdef TEST_GUI
    extern gui_test();
    gui_test();
#endif

    user_first_process_run();

    run();
}




_START static void init(multiboot_info_t* mb)
{


    int_init();

    fb_init(mb);

    mm_init(mb);


    // never to here
    return;

}

static void run()
{
    idle_process(0);
}

static void parse_kernel_cmdline()
{
    char* cmd = strdup((char*)g_mb->cmdline);
    char *token, *end;
    token = cmd;
    memset(&TestControl, 0, sizeof(TestControl));

#define SKIP_WHILE(t)\
    do{\
        while (*t == ' ' || *t == '\t')\
            t++;\
    }while(0)

#define SKIP_CHAR(t)\
    do{\
        while (*t != ' ' && *t != '\t' && *t != '\0')\
            t++;\
    }while(0)

    do 
    {
        SKIP_WHILE(token);
        end = token;
        SKIP_CHAR(end);
        if (*token == '\0')
            break;
        *end = '\0';
        end++;
        if (strcmp(token, "test") == 0)
            TestControl.test_enable = 1;

        if (strcmp(token, "all") == 0)
            TestControl.test_all = 1;

        if (strcmp(token, "mmap") == 0)
            TestControl.test_mmap = 1;

        if (strcmp(token, "mm") == 0)
            TestControl.test_mm= 1;

        if (strcmp(token, "fs_read") == 0)
            TestControl.test_fs_read = 1;

        if (strcmp(token, "fs_write") == 0)
            TestControl.test_fs_write = 1;

        if (strcmp(token, "ffs") == 0)
        {
            TestControl.test_fs_read = 1;
            TestControl.test_ffs = 1;
        }

        token = end;
    } while (1);

}
