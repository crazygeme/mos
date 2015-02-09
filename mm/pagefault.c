#include <int/int.h>
#include <mm/pagefault.h>
#include <mm/mm.h>
#include <ps/ps.h>
#include <mm/mmap.h>
#include <fs/namespace.h>

static void pf_process(intr_frame* frame);

void pf_init()
{
	int_register(0xe, pf_process, 0, 0);

}

/*
31                4                             0
+-----+-...-+-----+-----+-----+-----+-----+-----+
|     Reserved    | I/D | RSVD| U/S | W/R |  P  |
+-----+-...-+-----+-----+-----+-----+-----+-----+ 
 
*/
#define PF_MASK_P       0x00000001
#define PF_MASK_RW      0x00000002
#define PF_MASK_US      0x00000004
#define PF_MASK_RSVD    0x00000008
static void pf_process(intr_frame* frame)
{
    unsigned cr2;
    unsigned cr3;
    unsigned error = frame->error_code;
    task_struct* cur = CURRENT_TASK();
    unsigned this_begin;
    int is_read;
    __asm__("movl %cr2, %eax");
    __asm__("movl %%eax, %0" : "=m"(cr2));

    __asm__("movl %cr3, %eax");
    __asm__("movl %%eax, %0" : "=m"(cr3));

    is_read = ((error & PF_MASK_RW) == 0);
    if ((error & PF_MASK_P) == 0) {
        vm_region* region;
        unsigned this_offset;
        INODE fd;

        this_begin = cr2 & PAGE_SIZE_MASK;

        region = vm_find_map(cur->user.vm, this_begin); 
        if (region) {
            fd = region->node; 
            this_offset = region->offset + (this_begin - region->begin);
            mm_add_dynamic_map(this_begin, 0, PAGE_ENTRY_USER_DATA);
            //RELOAD_CR3(cr3);
            memset(this_begin, 0, PAGE_SIZE);
            if (fd != 0) {
                vfs_read_file(fd, this_offset, this_begin, PAGE_SIZE);
            }
            return;
        } else {
            printk("page fault! error code %x, address %x, eip %x\n", frame->error_code, cr2, frame->eip);
        }
    } else {
        printk("page fault! error code %x, address %x, eip %x\n", frame->error_code, cr2, frame->eip);
    }

	for(;;)
    {
        __asm__("hlt");
    }
}
