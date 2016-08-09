#include <klib.h>
#include <tty.h>
#include <timer.h>
#include <lock.h>
#include <ps.h>
#include <vfs.h>;
#include <mm.h>
#include <multiboot.h>
#include <list.h>
#include <errno.h>
#include <mmap.h>
#include <phymm.h>

extern unsigned long long phy_mem_low;
extern unsigned long long phy_mem_high;

extern unsigned phymm_cur;
extern unsigned phymm_high;
extern unsigned phymm_max;
extern unsigned phymm_valid;

void malloc_test()
{
#define MIN_MEM 1
#define MAX_MEM 4080
#define ALLOC_COUNT 10
    // test kmalloc, kfree
    void* addr[ALLOC_COUNT];
    int size = 0;
    int mallocor = 10;
    int* a = kmalloc(1);
    int* b = kmalloc(25);
    int i = 0;
    unsigned count = 0;
    kfree(a);
    kfree(b);
    while (1)
    {
        int mem_count = klib_rand() % ALLOC_COUNT;
        if (mem_count == 0)
            mem_count = 1;

        count++;
        for (i = 0; i < mem_count; i++)
        {
            size = klib_rand() % (MAX_MEM - MIN_MEM);
            size += MIN_MEM;
            if (size == 0)
                addr[i] = 0;
            else
            {
                addr[i] = kmalloc(size);
                if (addr[i])
                    memset(addr[i], 0xc, size);
            }
        }
        for (i = 0; i < mem_count; i++)
        {
            kfree(addr[i]);
        }

        if ((count % 1000) == 0)
        {
            mm_report();
            klogquota();
        }
    }
}

void mm_test()
{
    unsigned int phy_page_low = phy_mem_low / PAGE_SIZE;
    unsigned int phy_page_high = phy_mem_high / PAGE_SIZE;
    printk("mask of 8M -1: %x\n", mm_get_phy_page_mask(2 * 1024 - 1));
    printk("mask of 8M: %x \n", mm_get_phy_page_mask(2 * 1024));
    printk("mask of high bound -1: %x, high bound is %x \n",
        mm_get_phy_page_mask(phy_page_high - 1),
        phy_page_high);
    printk("mask of high bound: %x, high bound is %x \n",
        mm_get_phy_page_mask(phy_page_high),
        phy_page_high);

    printk("mask of high bound +1: %x, high bound is %x \n",
        mm_get_phy_page_mask(phy_page_high + 1),
        phy_page_high);

    printk("first free page index is %x\n", mm_get_free_phy_page_index());

    if (1)
    {
        int i = 0;
        unsigned int *table1, *table2;
        for (i = 0; i < 5; i++)
        {
            table1 = mm_alloc_page_table();
            table2 = mm_alloc_page_table();
            printk("alloc page1 %x, page 2 %x\n", table1, table2);
            printk("table1[0] %x, table1[1] %x\n", table1[0], table1[1]);
            printk("table2[0] %x, table2[1] %x\n", table2[0], table2[1]);

            mm_free_page_table(table2);
            mm_free_page_table(table1);
        }
    }

    if (1)
    {
        unsigned int vm = vm_alloc(4);
        unsigned int vm2 = vm_alloc(6);
        printk("vm alloc %x, vm2 %x\n", vm, vm2);
        vm_free(vm, 4);
        vm_free(vm2, 6);
    }
}

