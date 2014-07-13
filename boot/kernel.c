#include <lib/klib.h>
#include <int/int.h>
#include <drivers/keyboard.h>
#include <mm/mm.h>
#include <boot/multiboot.h>
#include <int/dsr.h>
#include <int/timer.h>
#include <ps/ps.h>
#include <drivers/block.h>
#include <drivers/hdd.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <syscall/syscall.h>
#include <mm/pagefault.h>
#include <fs/console.h>
#include <fs/kbchar.h>
#include <fs/null.h>
#include <fs/cache.h>
#include <fs/pipechar.h>
#include <drivers/serial.h>

static void run(void);
_START static void init(multiboot_info_t* mb);

_START void kmain(multiboot_info_t* mb, unsigned int magic)
{

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        return;
    }


    init(mb);


	// never to here
	
	return;
}


static void kmain_process(void* param);
static void idle_process(void* param);

void kmain_startup()
{
    int i = 0;
    
    // after klib_init, kmalloc/kfree/prink/etc are workable    
    klib_init();

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


#ifdef TEST_MALLOC
	extern test_malloc_process();
	test_malloc_process();
#endif

#ifdef TEST_MM
	mm_test();
#endif

        printk("Init process\n");
	ps_init();

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
    ps_create(idle_process, 0, 0, ps_kernel);
    // create first process
    ps_create(kmain_process, 0, 1, ps_kernel);
    ps_kickoff();

    // never going to here because ps0 never exit
#endif
#endif


	run();

}

static void idle_process(void* param)
{
    while (1) {
        printk("idle!\n");
        __asm__("hlt");
    }
}

static void kmain_process(void* param)
{
    klog_init();

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


    printk("Init file cache for libc, it take times...\n");
    file_cache_init();
    printk("Cache init done\n");

#ifdef TEST_BLOCK
	extern void test_block_process();
	test_block_process();
#endif

#ifdef TEST_MOUNT
    extern void dump_mount_point();
    dump_mount_point();
#endif

#ifdef TEST_NS
	extern void test_ns();
	test_ns();
#endif

    printk("Init system call table\n");
    syscall_init();


    klib_clear();
    user_first_process_run();

    run();
}




_START static void init(multiboot_info_t* mb)
{


    int_init();

    mm_init(mb);
	
	
	// never to here
	return;

}

static void run()
{

	while(1){
    }
}
