
#include "klib.h"
#include "int.h"
#include "keyboard.h"
#include "mm.h"
#include "multiboot.h"
#include "dsr.h"
#include "timer.h"
#include "ps.h"
#include "drivers/block.h"
#include "drivers/hdd.h"
#include "fs/vfs.h"
#include "fs/mount.h"

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
	
	

	int_enable_all();

	mm_del_user_map();

	dsr_init();


	kb_init();
	
	timer_init();

    klib_init();

    // now we are debuggable
    // printk("hello from %d, %u, %x, %s\n", -100, -100, -100, "world");


    timer_calculate_cpu_cycle();


#ifdef TEST_MALLOC
	extern test_malloc_process();
	test_malloc_process();
#endif

#ifdef TEST_MM
	mm_test();
#endif

	ps_init();


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
    // FIXME
    // make it ps_user
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

#ifdef TEST_BLOCK
	extern void test_block_process();
	test_block_process();
#endif

#ifdef TEST_MOUNT
    extern void dump_mount_point();
    dump_mount_point();
#endif

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
