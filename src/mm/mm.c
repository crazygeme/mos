#include <mm.h>
#include <klib.h>
#include <multiboot.h>
#include <list.h>
#include <lock.h>
#include <ps.h>
#include <timer.h>
#include <errno.h>
#include <mmap.h>
#include <phymm.h>

#define GDT_ADDRESS			0x1C0000
_START static void mm_get_phy_mem_bound(multiboot_info_t* mb);
_START static void mm_setup_beginning_8m();
static void mm_clear_beginning_user_map();

unsigned long long phy_mem_low;
unsigned long long phy_mem_high;

unsigned phymm_max = 0;
unsigned phymm_valid = 0;

_STARTDATA static unsigned long long _phy_mem_low;
_STARTDATA static unsigned long long _phy_mem_high;


short pgc_entry_count[1024];

_START void mm_init(multiboot_info_t* mb)
{
    mm_get_phy_mem_bound(mb);
    mm_setup_beginning_8m();
}


_START static void mm_get_phy_mem_bound(multiboot_info_t* mb)
{
    unsigned int map_addr = mb->mmap_addr;
    memory_map_t *map;

    if (mb->flags & 0x40)
    {

        map = (memory_map_t*)mb->mmap_addr;
        while ((unsigned int)map < mb->mmap_addr + mb->mmap_length)
        {

            if (map->type == 0x1 && map->base_addr_low != 0)
            {
                _phy_mem_low = map->base_addr_low + map->base_addr_high << 32;
                _phy_mem_high = phy_mem_low + (map->length_low + map->length_high << 32);
                break;
            }
            map = (memory_map_t*)((unsigned int)map + map->size + sizeof(unsigned int));
        }
    }
}



#define ENABLE_PAGING() \
    __asm__ ("movl %cr0,%eax"); \
    __asm__ ("orl $0x80000000,%eax"); \
    __asm__ ("movl %eax,%cr0");

#define RELOAD_EIP() \
	__asm__ ("jmp 1f \n1:\n\tmovl $1f,%eax\n\tjmp *%eax \n1:\n\tnop");

#define RELOAD_ESP(OFFSET) \
	__asm__ ("movl %esp, %ecx");\
	__asm__ ("addl %0, %%ecx" : : "i" (OFFSET));\
	__asm__ ("movl %ecx, %esp");
 


unsigned mm_get_pagedir()
{
    unsigned cr3 = 0;
    __asm__("movl %%cr3, %0" : "=r"(cr3));
    return cr3 + KERNEL_OFFSET;
}

static STACK page_table_cache;
// take 8M ~ 12M as page table cache
static void mm_init_page_table_cache()
{
    int i = 0;

    InitializeStack(&page_table_cache);

    for (i = 0; i < 1024; i++)
    {
        pgc_entry_count[i] = 0;
    }
}


static spinlock mm_lock;
static void lock_mm()
{
    spinlock_lock(&mm_lock);
}

static void unlock_mm()
{
    spinlock_unlock(&mm_lock);
}

static void mm_high_memory_fun()
{
    // ok now we make eip as virtual address
    RELOAD_EIP();
    phy_mem_high = _phy_mem_high;
    phy_mem_low = _phy_mem_low;
    phymm_max = (phy_mem_high) / PAGE_SIZE;
    // ok now we make esp as virtual address
    RELOAD_ESP(KERNEL_OFFSET);

    phymm_valid = phymm_get_mgmt_pages(phy_mem_high);
    phymm_valid += (phy_mem_low / PAGE_SIZE + RESERVED_PAGES);

    mm_init_page_table_cache();

    // physical memory management
    phymm_setup_mgmt_pages(phy_mem_low / PAGE_SIZE + RESERVED_PAGES);

    spinlock_init(&mm_lock);
    extern kmain_startup();
    kmain_startup();
}

static void mm_clear_beginning_user_map()
{
    unsigned int reserved_page_tables = (RESERVED_PAGES / 1024);
    unsigned int * gdt = (int*)(GDT_ADDRESS + KERNEL_OFFSET);
    int i;

    for (i = 0; i < reserved_page_tables; i++)
    {
        gdt[i] = 0;
    }

}

_START static void mm_setup_beginning_8m()
{
    unsigned int phy = PAGE_ENTRY_USER_DATA;
    unsigned int reserved_page_tables = (RESERVED_PAGES / 1024);
    unsigned int pg = (GDT_ADDRESS + PAGE_SIZE);
    unsigned int * pgt;
    int i = 0, j = 0;
    unsigned int * gdt = (int*)GDT_ADDRESS;

    for (i = 0; i < PG_TABLE_SIZE; i++)
    {
        gdt[i] = 0;
    }
    for (i = 0; i < reserved_page_tables; i++)
    {
        gdt[i] = (pg + i * PAGE_SIZE) | PAGE_ENTRY_USER_DATA;
        gdt[i + 768] = (pg + i * PAGE_SIZE) | PAGE_ENTRY_USER_DATA;
    }

    for (j = 0; j < reserved_page_tables; j++)
    {
        pgt = (unsigned int *)(pg + j * PAGE_SIZE);
        for (i = 0; i < PE_TABLE_SIZE; i++)
        {
            pgt[i] = phy;
            phy += PAGE_SIZE;
        }
    }

    RELOAD_CR3(GDT_ADDRESS);
    ENABLE_PAGING();
    mm_high_memory_fun();
    return;
}

int mm_add_resource_map(unsigned int phy)
{
    unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(phy);
    unsigned int page_table_offset = ADDR_TO_PET_OFFSET(phy);
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();


    if (phy < KERNEL_OFFSET)
    {
        return -1;
    }

    if ((page_dir[page_dir_offset] & PAGE_SIZE_MASK) == 0)
    {
        unsigned int table_addr = mm_alloc_page_table();
        if (table_addr == 0)
        {
            return -1;
        }

        page_dir[page_dir_offset] = (table_addr - KERNEL_OFFSET) | PAGE_ENTRY_KERNEL_DATA;
    }

    {
        unsigned int *table = (unsigned int*)((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET);
        unsigned int phy_page = (phy)& PAGE_SIZE_MASK;

        table[page_table_offset] = phy_page | PAGE_ENTRY_KERNEL_DATA;
        return 1;
    }
}

int mm_add_direct_map(unsigned int vir)
{
    unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    unsigned int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
    unsigned int page_busy = 0;
    unsigned int page_index = 0;
    // only kernel space has direct map
    if (vir < KERNEL_OFFSET)
        return -1;

    if (vir >= 0xC0000000 && vir < PAGE_TABLE_CACHE_END)
        return 1;

    page_index = (vir - KERNEL_OFFSET) & PAGE_SIZE_MASK;
    page_index = page_index / PAGE_SIZE;
    page_busy = phymm_is_used(page_index);
    if (page_busy)
    {
        return -1;
    }

    if ((page_index * PAGE_SIZE) > KERNEL_SIZE)
        return -1;

    if ((page_dir[page_dir_offset] & PAGE_SIZE_MASK) == 0)
    {
        unsigned int table_addr = mm_alloc_page_table();
        if (table_addr == 0)
        {
            return -1;
        }

        page_dir[page_dir_offset] = (table_addr - KERNEL_OFFSET) | PAGE_ENTRY_KERNEL_DATA;
    }

    {
        unsigned int *table = (unsigned int*)((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET);
        unsigned int phy_page = (vir - KERNEL_OFFSET) & PAGE_SIZE_MASK;
        unsigned int entry = table[page_table_offset] & PAGE_SIZE_MASK;

        if (!entry)
        {
            int idx = (PAGE_TABLE_CACHE_END - (unsigned)table) / PAGE_SIZE - 1;
            pgc_entry_count[idx]++;
        }
        table[page_table_offset] = phy_page | PAGE_ENTRY_KERNEL_DATA;
        phymm_reference_page(phy_page / PAGE_SIZE);
        if (vir == ((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET))
            return 1;
        else
            return 0;
    }
}

void mm_del_direct_map(unsigned int vir)
{
    int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
    unsigned int *page_table, phy;
    unsigned int phy_addr = (vir >= KERNEL_OFFSET) ?
        ((vir&PAGE_SIZE_MASK) - KERNEL_OFFSET) : (vir&PAGE_SIZE_MASK);
    int empty = 1;
    int i = 0, idx;

    // 0xC0000000 ~ PAGE_TABLE_CACHE_END is always mapped
    if (vir >= 0xC0000000 && vir < PAGE_TABLE_CACHE_END)
        return;


    page_table = (unsigned int*)((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET);

    phy = page_table[page_table_offset] & PAGE_SIZE_MASK;

    page_table[page_table_offset] = 0;
    phymm_dereference_page(phy_addr / PAGE_SIZE);

    if (phy)
    {
        idx = (PAGE_TABLE_CACHE_END - (unsigned)page_table) / PAGE_SIZE - 1;
        pgc_entry_count[idx]--;
        empty = (pgc_entry_count[idx] == 0);
    }
    else
    {
        empty = 0;
    }


    if (empty)
    {
        mm_free_page_table((unsigned int)page_table);
        page_dir[page_dir_offset] = 0;
    }
}


void mm_del_user_map()
{
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
    page_dir[0] = 0;
    page_dir[1] = 0;
}

unsigned int mm_alloc_page_table()
{
    unsigned int *ret = PopStack(&page_table_cache);
    task_struct* cur = CURRENT_TASK();
    if (ret == 0)
        return ret;
    if (ret < 0xC0000000)
    {
        printk("[%d] alloc page table %x\n", cur->psid, ret);
    }
    memset(ret, 0, PAGE_SIZE);
    return ret;
}

void mm_free_page_table(unsigned int vir)
{
    int ret = PushStack(&page_table_cache, vir);

    if (vir < 0xC0000000)
    {
        printk("free page table %x\n", vir);
    }

    if (!ret)
    {
        printk("fatal error: stack overflow\n");
    }
}

unsigned int vm_alloc(int page_count)
{
    int page_index = 0;
    int i = 0;
    int valid = 0;
    unsigned int vir = 0;

    lock_mm();

    page_index = phymm_alloc_kernel(page_count);

    for (i = 0; i < page_count; i++)
    {
        vir = (page_index + i) * PAGE_SIZE + KERNEL_OFFSET;
        mm_add_direct_map(vir);
        memset(vir, 0, PAGE_SIZE);
    }

    vir = page_index * PAGE_SIZE + KERNEL_OFFSET;
    unlock_mm();
    return vir;

}

void vm_free(unsigned int vm, int page_count)
{
    unsigned int phy = 0;
    int i = 0;
    unsigned int cur;
    lock_mm();
    vm = vm & PAGE_SIZE_MASK;
    for (i = 0; i < page_count; i++)
    {
        cur = vm + i * PAGE_SIZE;
        phy = cur - KERNEL_OFFSET;
        mm_del_direct_map(cur);
    }

    phymm_free_kernel((vm-KERNEL_OFFSET)/PAGE_SIZE, page_count);

    unlock_mm();

}

int mm_add_dynamic_map(unsigned int vir, unsigned int phy, unsigned flag)
{
    unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    unsigned int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
    unsigned int page_busy = 0;
    unsigned int page_index = 0;
    unsigned int target_phy = 0;

    if (phy)
    {
        phy = phy & PAGE_SIZE_MASK;
        page_index = phy / PAGE_SIZE;
        target_phy = phy;
    }
    else
    {
        page_index = phymm_alloc_user();
        target_phy = page_index*PAGE_SIZE;
    }


    if ((page_dir[page_dir_offset] & PAGE_SIZE_MASK) == 0)
    {
        unsigned int table_addr = mm_alloc_page_table();
        if (table_addr == 0)
        {
            return -1;
        }

        page_dir[page_dir_offset] = (table_addr - KERNEL_OFFSET) | flag;
        page_dir[page_dir_offset] |= PAGE_ENTRY_WRITABLE;
    }

    {
        unsigned int *table = (unsigned int*)((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET);
        unsigned int phy_page = (target_phy)& PAGE_SIZE_MASK;
        unsigned int table_entry = table[page_table_offset] & PAGE_SIZE_MASK;
        if (table_entry)
        {
            return 0;
        }
        else
        {
            int idx = (PAGE_TABLE_CACHE_END - (unsigned)table) / PAGE_SIZE - 1;
            pgc_entry_count[idx]++;
        }

        table[page_table_offset] = phy_page | flag;
        phymm_reference_page(phy_page / PAGE_SIZE);

    }
    return 1;
}

void mm_del_dynamic_map(unsigned int vir)
{
    int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
    unsigned int *page_table;
    unsigned int phy_addr;
    int empty = 1;
    int i = 0, idx;


    page_table = (unsigned int*)((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET);
    phy_addr = page_table[page_table_offset] & PAGE_SIZE_MASK;

    page_table[page_table_offset] = 0;



    if (phy_addr)
    {
        if (phymm_dereference_page(phy_addr / PAGE_SIZE) == 0)
            phymm_free_user(phy_addr / PAGE_SIZE);
        idx = (PAGE_TABLE_CACHE_END - (unsigned)page_table) / PAGE_SIZE - 1;
        pgc_entry_count[idx]--;
        empty = (pgc_entry_count[idx] == 0);
    }
    else
    {
        empty = 0;
    }



    if (empty)
    {
        mm_free_page_table((unsigned int)page_table);
        page_dir[page_dir_offset] = 0;
    }

}

unsigned mm_get_map_flag(unsigned vir)
{
    int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
    unsigned int *page_table;
    unsigned flag;

    page_table = (unsigned int*)((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET);
    flag = page_table[page_table_offset];
    flag &= ~PAGE_SIZE_MASK;
    return flag;
}

void mm_set_map_flag(unsigned vir, unsigned flag)
{
    int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
    unsigned int *page_table;

    page_table = (unsigned int*)((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET);
    page_table[page_table_offset] &= PAGE_SIZE_MASK;
    page_table[page_table_offset] |= flag;
}

unsigned mm_get_attached_page_index(unsigned int vir)
{
    int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int *page_dir = (unsigned int*)mm_get_pagedir();
    unsigned int *page_table;
    unsigned int phy_addr;

    page_table = (unsigned int*)((page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET);
    phy_addr = page_table[page_table_offset] & PAGE_SIZE_MASK;

    return (phy_addr / PAGE_SIZE);
}


unsigned vm_get_usr_zone(unsigned page_count)
{
    // FIXME
    // only used in load elf, please modify it
    task_struct* cur = CURRENT_TASK();
    unsigned begin = vm_disc_map(cur->user.vm, page_count*PAGE_SIZE);

    return begin;
}
