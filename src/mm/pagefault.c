#include <int.h>
#include <pagefault.h>
#include <mm.h>
#include <ps.h>
#include <mmap.h>
#include <namespace.h>

unsigned page_fault_count;
unsigned page_falut_total_time;

static void pf_process(intr_frame* frame);

void pf_init()
{
    page_fault_count = 0;
    page_falut_total_time = 0;
    int_register(0xe, pf_process, 0, 0);
}

/*
31                4                             0
+-----+-...-+-----+-----+-----+-----+-----+-----+
|     Reserved    | I/D | RSVD| U/S | W/R |  P  |
+-----+-...-+-----+-----+-----+-----+-----+-----+

*/
#define PF_MASK_P       0x00000001  // page valid
#define PF_MASK_RW      0x00000002  // write access
#define PF_MASK_US      0x00000004  // user mode access
#define PF_MASK_RSVD    0x00000008
static void pf_process(intr_frame* frame)
{
    unsigned cr2;
    unsigned cr3;
    unsigned error = frame->error_code;
    task_struct* cur = CURRENT_TASK();
    unsigned this_begin;
    unsigned vir;
    unsigned tmp;
    int page_valid;
    int write_access;
    int user_mode;
    unsigned oldint;
    unsigned begin = time_now();

    oldint = int_intr_disable();

    __asm__("movl %cr2, %eax");
    __asm__("movl %%eax, %0" : "=m"(cr2));

    __asm__("movl %cr3, %eax");
    __asm__("movl %%eax, %0" : "=m"(cr3));

    page_fault_count++;
    page_valid = ((error & PF_MASK_P) == PF_MASK_P);
    write_access = ((error & PF_MASK_RW) == PF_MASK_RW);
    user_mode = ((error & PF_MASK_US) == PF_MASK_US);

    if (!page_valid)
    {
        vm_region* region;
        unsigned this_offset;
        INODE fd;

        this_begin = cr2 & PAGE_SIZE_MASK;

        region = vm_find_map(cur->user.vm, this_begin);
        if (region)
        {
            fd = region->node;
            this_offset = region->offset + (this_begin - region->begin);
            mm_add_dynamic_map(this_begin, 0, PAGE_ENTRY_USER_DATA);
            memset(this_begin, 0, PAGE_SIZE);
            if (fd != 0)
            {
                vfs_read_file(fd, this_offset, this_begin, PAGE_SIZE);
            }
            goto Done;
        }
        else
        {
            printk("page fault! error code %x, address %x, eip %x\n", frame->error_code, cr2, frame->eip);
        }
    }
    else if (write_access)
    {
        unsigned page_index = mm_get_attached_page_index(cr2);
        task_struct* cur = CURRENT_TASK();
        unsigned cow;
        unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(cr2);
        unsigned int page_table_offset = ADDR_TO_PET_OFFSET(cr2);
        unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
        unsigned flag;

        cow = phymm_is_cow(page_index);
        // klog("ps %d write %x fault, cow %d\n", cur->psid, page_index, cow);
        if (cow)
        {
            vir = cr2;
            tmp = vm_alloc(1);
            memcpy(tmp, (vir&PAGE_SIZE_MASK), PAGE_SIZE);
            flag = mm_get_map_flag(vir);
            flag |= PAGE_ENTRY_PRESENT;
            flag |= PAGE_ENTRY_WRITABLE;
            mm_del_dynamic_map(vir);
            mm_add_dynamic_map(vir, tmp - KERNEL_OFFSET, flag);
            vm_free(tmp, 1);
            RELOAD_CR3(cr3);
        }
        else
        {
            flag = mm_get_map_flag(cr2);
            flag |= PAGE_ENTRY_WRITABLE;
            mm_set_map_flag(cr2, flag);
        }

        goto Done;
    }
    else
    {
        printk("page fault! error code %x, address %x, eip %x\n", frame->error_code, cr2, frame->eip);
    }

    for (;;)
    {
        __asm__("hlt");
    }

Done:
    if (oldint)
    {
        int_intr_enable();
    }

    page_falut_total_time += (time_now() - begin);
}
