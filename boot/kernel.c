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

void kmain_startup()
{
    int i = 0;
    
    // after klib_init, kmalloc/kfree/prink/etc are workable    
    klib_init();

    dsr_init();

    int_enable_all();

    mm_del_user_map();

    kb_init();
	
    timer_init();



    timer_calculate_cpu_cycle();


#ifdef TEST_MALLOC
	extern test_malloc_process();
	test_malloc_process();
#endif

#ifdef TEST_MM
	mm_test();
#endif

	ps_init();

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
    // create first process
    ps_create(kmain_process, 0, 1, ps_kernel);
    ps_kickoff();

    // never going to here because ps0 never exit
#endif
#endif


	run();

}


static void kmain_process(void* param)
{
    vfs_init();

    mount_init();

    block_init();

    hdd_init();

    vfs_trying_to_mount_root();

    chardev_init();

    kbchar_init();

    console_init();

    null_init();

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

    syscall_init();

    user_first_process_run();

    // we never here
    printk("idle\n");
    while (1) {
        // this can become wait syscall
        ps_cleanup_dying_task();
    }
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
