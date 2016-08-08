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

_START static void mm_dump_phy(multiboot_info_t* mb);
_START static void mm_get_phy_mem_bound(multiboot_info_t* mb);
_START static void mm_setup_beginning_8m();
static void mm_clear_beginning_user_map();

static unsigned long long phy_mem_low;
static unsigned long long phy_mem_high;

unsigned phymm_cur = 0;
unsigned phymm_high = 0;
unsigned phymm_max = 0;
unsigned phymm_valid = 0;

_STARTDATA static unsigned long long _phy_mem_low;
_STARTDATA static unsigned long long _phy_mem_high;

// this can present 8*32*1024*4k = 1G
#define PAGE_MASK_TABLE_SIZE (8*1024)
static unsigned free_phy_page_mask[PAGE_MASK_TABLE_SIZE];

short pgc_entry_count[1024];

_START void mm_init(multiboot_info_t* mb)
{
    // mm_dump_phy(mb);
    mm_get_phy_mem_bound(mb);
    mm_setup_beginning_8m();
}



_START void mm_dump_phy(multiboot_info_t* mb)
{
    unsigned int count = mb->mmap_length;
    unsigned int map_addr = mb->mmap_addr;
    unsigned int i = 0;
    memory_map_t *map;
    klib_info("flags: ", mb->flags, "\n");

    klib_info("mem_lower: ", mb->mem_lower, "\t");

    klib_info("mem_upper: ", mb->mem_upper, "\t");

    klib_info("boot_device: ", mb->boot_device, "\n");

    klib_info("cmdline: ", mb->cmdline, "\t");

    klib_info("mods_count: ", mb->mods_count, "\t");

    klib_info("mods_addr: ", mb->mods_addr, "\n");

    klib_info("mmap_length: ", mb->mmap_length, "\t");

    klib_info("mmap_addr: ", mb->mmap_addr, "\n");

    if (mb->flags & 0x40)
    {

        map = mb->mmap_addr;
        while ((unsigned int)map < mb->mmap_addr + mb->mmap_length)
        {

            klib_info("base_addr: ", map->base_addr_low, ":");
            klib_info("", map->base_addr_high, "\t");
            klib_info("length: ", map->length_low, ":");
            klib_info("", map->length_high, "\t");
            klib_info("type: ", map->type, "\n");

            map = (memory_map_t*)((unsigned int)map + map->size + sizeof(unsigned int));
        }
    }

    /*
    dump info when set -m 256
    flags: 0x4F
    mem_lower: 0x27F    mem_upper: 0x3FBF8  boot_device: 0x8000FFFF
    cmdline: 0x109000   mods_count: 0x0 mods_addr: 0x109000
    mmap_length: 0x90   mmap_addr: 0x9000
    base_addr: 0x0:0x0          length: 0x9FC00:0x0     type: 0x1  ; first 640K reserved for booting
    base_addr: 0x9FC00:0x0      length: 0x400:0x0       type: 0x2
    base_addr: 0xF0000:0x0      length: 0x10000:0x0     type: 0x2
    base_addr: 0x100000:0x0     length: 0xFEFE000:0x0   type: 0x1  ; usable memory
    base_addr: 0xFFFE000:0x0    length: 0x2000:0x0      type: 0x2
    base_addr: 0xFFFC0000:0x0   length: 0x40000:0x0     type: 0x2
    */
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

    // klib_info("static address: ", &phy_mem_low, "\n");
    // klib_info("local address: ", &map_addr, "\n");
    // klib_info("code address: ", &mm_get_phy_mem_bound, "\n");
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


_START static void simulate_paging(unsigned address)
{
    int *gdt = (int*)GDT_ADDRESS;
    int *pet = 0;
    int gd_index = 0;
    int pe_index = 0;
    int page_offset = 0;
    unsigned phy = 0;


    klib_info("virtual address: ", address, "\n");

    gd_index = ADDR_TO_PGT_OFFSET(address);
    pe_index = ADDR_TO_PET_OFFSET(address);
    page_offset = ADDR_TO_PAGE_OFFSET(address);

    klib_info("gd_index: ", gd_index, "\n");
    klib_info("pe_index: ", pe_index, "\n");
    klib_info("page_offset: ", page_offset, "\n");

    pet = (int*)(gdt[0] & 0xfffff000);
    klib_info("pet: ", pet, "\n");

    phy = pet[pe_index] & 0xfffff000;
    klib_info("phy: ", phy, "\n");

    phy = phy | page_offset;
    klib_info("physical address: ", phy, "\n");
}

static unsigned int mm_get_phy_page_mask(unsigned int page_index)
{
    int mask_index = page_index / 32;
    int mask_offset = page_index % 32;
    int mask = 1 << mask_offset;

    return ((free_phy_page_mask[mask_index] & mask));
}

void mm_set_phy_page_mask(unsigned int page_index, unsigned int used)
{
    int mask_index = page_index / 32;
    int mask_offset = page_index % 32;
    int mask = 1 << mask_offset;
    unsigned ref_count;
    int set = 0;
    unsigned int phy_page_low = phy_mem_low / PAGE_SIZE;
    unsigned int phy_page_high = phy_mem_high / PAGE_SIZE;


    if (used)
    {
        if (page_index >= phymm_valid && page_index < phy_page_high)
        {
            ref_count = phymm_reference_page(page_index);
            if (ref_count == 1)
            {
                free_phy_page_mask[mask_index] |= mask;
                set = 1;
            }
        }
        else
        {
            free_phy_page_mask[mask_index] |= mask;
            set = 1;
        }

        if (set)
        {

            if (page_index > phy_page_low && page_index < phy_page_high)
            {
                phymm_cur += PAGE_SIZE;
                if (phymm_cur > phymm_high)
                {
                    phymm_high = phymm_cur;
                }
            }
        }
    }
    else
    {

        if (page_index >= phymm_valid && page_index < phy_page_high)
        {
            ref_count = phymm_dereference_page(page_index);
            if (ref_count == 0)
            {
                free_phy_page_mask[mask_index] &= ~mask;
                set = 1;
            }
        }
        else
        {
            // FIXME
            // before first process runs, kernel routine will map gdt[0 ...] and gdt[768...]
            // to same physical memory
            // this will make dup_user_map reference to reserved physical memory, because
            //      1. phy[0...] will ref to new process, with COW
            //      2. if cur write-access page fault occurs, cur will allocate new memory and point to it
            //      3. after that new process write-access page fault occures, just set this page as writable
            //      4. then phy[0...] belong to new process
            //      5. will set to free when new process end
            // should clear gdt[0...] in proper time
            // free_phy_page_mask[mask_index] &= ~mask; 
            set = 1;
        }

        if (set)
        {
            phymm_cur -= PAGE_SIZE;
        }
    }
}



static void mm_init_free_phy_page_mask()
{
    int i = 0;
    unsigned int phy_page_low = phy_mem_low / PAGE_SIZE;
    unsigned int phy_page_high = phy_mem_high / PAGE_SIZE;

    for (i = 0; i < PAGE_MASK_TABLE_SIZE; i++)
        free_phy_page_mask[i] = 0;

    // lower then phy_mem_low set to used
    for (i = 0; i < phy_page_low; i++)
        mm_set_phy_page_mask(i, 1);

    // higher then phy_mem_high set to used
    for (i = phy_page_high; i < (8 * PAGE_MASK_TABLE_SIZE); i++)
        mm_set_phy_page_mask(i, 1);

    for (i = phy_page_low; i < (phy_page_low + RESERVED_PAGES); i++)
        mm_set_phy_page_mask(i, 1);
}

static inline int first_not_set(int x)
{
    int r;

    x = ~x;
    asm("bsfl %1,%0\n\t"
        "cmovzl %2,%0"
        : "=r" (r) : "rm" (x), "r" (-1));
    return r;
}

unsigned int  mm_get_free_phy_page_index()
{
    unsigned int i = 0;
    unsigned int page_mask_index = 0;
    int mask = 0;
    int offset = 0;

    for (i = 0; i < PAGE_MASK_TABLE_SIZE; i++)
    {
        if (free_phy_page_mask[i] != 0xffffffff)
        {
            page_mask_index = i;
            break;
        }
    }

    if (i == PAGE_MASK_TABLE_SIZE)
        return -1;

    mask = free_phy_page_mask[page_mask_index];


    // find first bit '0' from least to highest
    offset = first_not_set(mask);

    if ((page_mask_index * 32 + offset) < 10)
    {
        klog("ops\n");

    }
    return (page_mask_index * 32 + offset);
}

#ifdef TEST_MM
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

    if (0)
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
#endif

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
    phymm_max = phy_mem_high - phy_mem_low;
    phymm_cur = phymm_high = 0;
    // ok now we make esp as virtual address
    RELOAD_ESP(KERNEL_OFFSET);

    phymm_valid = phymm_get_mgmt_pages(phy_mem_high);
    phymm_valid += (phy_mem_low / PAGE_SIZE + RESERVED_PAGES);

    mm_init_free_phy_page_mask();
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
    // simulate_paging(0x001012a0);
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
    page_busy = mm_get_phy_page_mask(page_index);
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
        mm_set_phy_page_mask(phy_page / PAGE_SIZE, 1);
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
    mm_set_phy_page_mask(phy_addr / PAGE_SIZE, 0);

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

    page_index = mm_get_free_phy_page_index();

    do
    {
        if ((page_index + page_count) >= 0x40000)
        {
            unlock_mm();
            return 0;
        }

        valid = 1;
        for (i = 0; i < page_count; i++)
        {
            if (mm_get_phy_page_mask(page_index + i) != 0)
            {
                valid = 0;
                break;
            }
        }
        if (!valid)
            page_index++;
    }
    while (!valid);

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
        page_index = mm_get_free_phy_page_index(); // vm_alloc(1) - KERNEL_OFFSET;
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
        mm_set_phy_page_mask(phy_page / PAGE_SIZE, 1);

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
        mm_set_phy_page_mask(phy_addr / PAGE_SIZE, 0);
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

#define MAP_SHARED      0x01            /* Share changes */
#define MAP_PRIVATE     0x02            /* Changes are private */
#define MAP_TYPE        0x0f            /* Mask for type of mapping */
#define MAP_FIXED       0x10            /* Interpret addr exactly */
#define MAP_ANONYMOUS   0x20            /* don't use a file */
#ifdef CONFIG_MMAP_ALLOW_UNINITIALIZED
# define MAP_UNINITIALIZED 0x4000000    /* For anonymous mmap, memory could be uninitialized */
#else
# define MAP_UNINITIALIZED 0x0          /* Don't support this flag */
#endif



int do_mmap(unsigned int _addr, unsigned int _len, unsigned int prot,
    unsigned int flags, int fd, unsigned int offset)
{
    unsigned addr = _addr & PAGE_SIZE_MASK;
    unsigned last_addr = (_addr + _len - 1) & PAGE_SIZE_MASK;
    unsigned page_count = (last_addr - addr) / PAGE_SIZE + 1;
    INODE node;
    task_struct* cur = CURRENT_TASK();

    if (_addr == 0)
    {
        addr = vm_disc_map(cur->user.vm, page_count*PAGE_SIZE);
    }

    // FIXME
    // lots of flags and prot
    if (fd != -1 && cur->fds[fd].flag != 0)
        node = cur->fds[fd].file;
    else
        node = 0;

    vm_add_map(cur->user.vm, addr, addr + page_count * PAGE_SIZE, node, offset);


    return addr;

}

int do_munmap(void *addr, unsigned length)
{
    task_struct* cur = CURRENT_TASK();
    vm_region* region = vm_find_map(cur->user.vm, addr);
    unsigned begin = ((unsigned)addr) & PAGE_SIZE_MASK;
    unsigned end = ((unsigned)addr + length - 1) & PAGE_SIZE_MASK;
    unsigned page_count = (end - begin) / PAGE_SIZE + 1;

    end = begin + page_count*PAGE_SIZE;
    if (!region)
    {
        return (-EINVAL);
    }


    if (begin > region->begin && end < region->end)
    {
        vm_del_map(cur->user.vm, addr);
        vm_add_map(cur->user.vm, region->begin, begin, region->node, region->offset);
        vm_add_map(cur->user.vm, end, region->end, region->node, region->offset + (end - region->begin));
    }
    else if (begin > region->begin && end == region->end)
    {
        vm_del_map(cur->user.vm, addr);
        vm_add_map(cur->user.vm, region->begin, begin, region->node, region->offset);
    }
    else if (begin == region->begin && end < region->end)
    {
        vm_del_map(cur->user.vm, addr);
        vm_add_map(cur->user.vm, end, region->end, region->node, region->offset + (end - region->begin));
    }
    else if (begin == region->begin && end == region->end)
    {
        vm_del_map(cur->user.vm, addr);
    }
    else
    {
        return (-EINVAL);
    }

    return 0;
}


