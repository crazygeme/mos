#include <int.h>
#include <pagefault.h>
#include <mm.h>
#include <ps.h>
#include <mmap.h>
#include <phymm.h>
#include <fs.h>

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
extern phymm_page* phymm_pages;
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
        filep f;
        ext4_file* ff;

        this_begin = cr2 & PAGE_SIZE_MASK;
        if (this_begin == 0)
        {
            goto HLT;
        } 

        region = vm_find_map(cur->user.vm, this_begin);
        if (region)
        {
            f = region->node;
            this_offset = region->offset + (this_begin - region->begin);
            mm_add_dynamic_map(this_begin, 0, PAGE_ENTRY_USER_DATA);
            REFRESH_CACHE();
            memset(this_begin, 0, PAGE_SIZE);
            if (f != 0)
            {
                int ret = 0;
                ff = f->inode;
                ret = ext4_fseek(ff, this_offset, SEEK_SET);
                if (ret != EOK){
                    klog("FAIL: mmap: seek to off %x\n", this_offset);
                }
                ret = ext4_fread(ff, this_begin, PAGE_SIZE, NULL);
                if (ret != EOK){
                    klog("FAIL: mmap: read to buffer %x, size %x\n", this_begin, PAGE_SIZE);
                }
            }
            goto Done;
        }
        else
        {
            goto HLT;
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
            vir = (vir&PAGE_SIZE_MASK);
            memcpy(cur->user.reserve, vir, PAGE_SIZE);
            flag = mm_get_map_flag(vir);
            flag |= PAGE_ENTRY_PRESENT;
            flag |= PAGE_ENTRY_WRITABLE;
            mm_del_dynamic_map(vir);
            mm_add_dynamic_map(vir, 0, flag);
            REFRESH_CACHE();
            memcpy(vir, cur->user.reserve, PAGE_SIZE);
            
        }
        else
        {
            vir = cr2;
            vir = (vir&PAGE_SIZE_MASK);
            flag = mm_get_map_flag(vir);
            flag |= PAGE_ENTRY_WRITABLE;
            mm_set_map_flag(vir, flag);
            REFRESH_CACHE();
        }

        goto Done;
    }
    else
    {
        goto HLT;
    }

HLT:
    do{
       task_struct* cur = CURRENT_TASK();
        printk("[%d]page fault! error code %x, address %x, eip %x\n", cur->psid, frame->error_code, cr2, frame->eip);
    }while(0);
 
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
