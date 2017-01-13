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

typedef struct _name_cache
{
    LIST_ENTRY list;
    void* buf;
}name_cache;

static name_cache name_cache_head;

static spinlock mm_lock;
static spinlock path_lock;
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
    spinlock_init(&path_lock);
    InitializeListHead(&name_cache_head);

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


static int mm_get_valid_page_table(unsigned addr, unsigned flag,
    unsigned** page_dir, 
    unsigned** dir_entry,
    unsigned** page_table,
    unsigned** table_entry)
{
    unsigned int *_page_dir;
    unsigned int *_dir_entry;
    unsigned int *_page_table;
    unsigned int *_table_entry;
    
    _page_dir = (unsigned int*)mm_get_pagedir();

    if ((_page_dir[ADDR_TO_PGT_OFFSET(addr)] & PAGE_SIZE_MASK) == 0){
        unsigned int table_addr = mm_alloc_page_table();
        if (table_addr == 0){
            return 0;
        }
        _page_dir[ADDR_TO_PGT_OFFSET(addr)] = (table_addr - KERNEL_OFFSET) | PAGE_ENTRY_KERNEL_DATA;
        _page_dir[ADDR_TO_PGT_OFFSET(addr)] |= flag;
    }
    _dir_entry = &(_page_dir[ADDR_TO_PGT_OFFSET(addr)]);
    _page_table = (unsigned int*)((_page_dir[ADDR_TO_PGT_OFFSET(addr)] & PAGE_SIZE_MASK) + KERNEL_OFFSET);
    _table_entry = &(_page_table[ADDR_TO_PET_OFFSET(addr)]);
    if (page_dir) *page_dir = _page_dir;
    if (dir_entry) *dir_entry = _dir_entry;
    if (page_table) *page_table = _page_table;
    if (table_entry) *table_entry = _table_entry;
    return 1;
}

static int mm_set_page_table_entry(unsigned addr, unsigned flag, unsigned value)
{
    unsigned int *table, *entry;
    unsigned int data;

    if (!mm_get_valid_page_table(addr, flag, NULL, NULL, &table, &entry))
        return 0;

    data = (*entry) & PAGE_SIZE_MASK;
    if (!data){
        int idx = (PAGE_TABLE_CACHE_END - (unsigned)table) / PAGE_SIZE - 1;
        pgc_entry_count[idx]++;
    }

    (*entry) = value;
    return 1;
}

static int mm_clear_page_table_entry(unsigned *dir_entry, unsigned page_table, unsigned* table_entry)
{
    unsigned phy;
    int idx, empty;
    phy = (*table_entry) & PAGE_SIZE_MASK;
    (*table_entry) = 0;
    if (phy)
    {
        idx = (PAGE_TABLE_CACHE_END - (unsigned)page_table) / PAGE_SIZE - 1;
        pgc_entry_count[idx]--;

    }

    if (pgc_entry_count[idx] == 0)
    {
        mm_free_page_table((unsigned int)page_table);
        *dir_entry = 0;
    }
}

int mm_add_resource_map(unsigned int phy)
{
    unsigned int *entry;
    unsigned int phy_page = (phy) & PAGE_SIZE_MASK;

    if (phy < KERNEL_OFFSET)
        return -1;

    if (!mm_get_valid_page_table(phy, 0, NULL, NULL, NULL, &entry))
        return -1;

    *entry = phy_page | PAGE_ENTRY_KERNEL_DATA;
    return 1;
}

int mm_add_direct_map(unsigned int vir)
{
    unsigned int *page_dir, *table, *entry;
    unsigned int phy_page = (vir - KERNEL_OFFSET) & PAGE_SIZE_MASK;
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
        return -1;

    if ((page_index * PAGE_SIZE) > KERNEL_SIZE)
        return -1;

    if (!mm_set_page_table_entry(vir, 0, phy_page | PAGE_ENTRY_KERNEL_DATA))
        return -1;

    phymm_reference_page(phy_page / PAGE_SIZE);
    return 1;
}

void mm_del_direct_map(unsigned int vir)
{
    unsigned int *page_table, *table_entry, *dir_entry;
    unsigned int phy;
    unsigned int phy_addr = (vir >= KERNEL_OFFSET) ?
        ((vir&PAGE_SIZE_MASK) - KERNEL_OFFSET) : (vir&PAGE_SIZE_MASK);
    int empty = 1;
    int i = 0, idx;

    // 0xC0000000 ~ PAGE_TABLE_CACHE_END is always mapped
    if (vir >= 0xC0000000 && vir < PAGE_TABLE_CACHE_END)
        return;

    if (!mm_get_valid_page_table(vir, 0, NULL, &dir_entry, &page_table, &table_entry))
        return;

    if (!mm_clear_page_table_entry(dir_entry, page_table, table_entry))
        return;
    

    phymm_dereference_page(phy_addr / PAGE_SIZE);
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
    memset(ret, 0, PAGE_SIZE);
    return ret;
}

void mm_free_page_table(unsigned int vir)
{
    PushStack(&page_table_cache, vir);
}

unsigned int vm_alloc(int page_count)
{
    int page_index = 0;
    int i = 0;
    int valid = 0;
    unsigned int vir = 0;

    lock_mm();

    page_index = phymm_alloc_kernel(page_count);
    if (page_index < 0)
        return NULL;

    for (i = 0; i < page_count; i++)
    {
        vir = (page_index + i) * PAGE_SIZE + KERNEL_OFFSET;
        mm_add_direct_map(vir);
    }
    REFRESH_CACHE();
    vir = page_index * PAGE_SIZE + KERNEL_OFFSET;
    memset(vir, 0, page_count * PAGE_SIZE);
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
    REFRESH_CACHE();
    phymm_free_kernel((vm-KERNEL_OFFSET)/PAGE_SIZE, page_count);

    unlock_mm();

}

int mm_add_dynamic_map(unsigned int vir, unsigned int phy, unsigned flag)
{
    unsigned int phy_page;
    unsigned int *page_dir, *table, *entry;
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

    phy_page = (target_phy)& PAGE_SIZE_MASK;

    if (!mm_set_page_table_entry(vir, (flag | PAGE_ENTRY_WRITABLE), phy_page | flag))
        return -1;

    phymm_reference_page(phy_page / PAGE_SIZE);

    return 1;
}

void mm_del_dynamic_map(unsigned int vir)
{
    unsigned int *page_table, *table_entry, *dir_entry;
    unsigned int phy_addr;
    int empty = 1;
    int i = 0, idx;
    int page_index;
    
    if (!mm_get_valid_page_table(vir, 0, NULL, &dir_entry, &page_table, &table_entry))
        return;

    phy_addr = (*table_entry) & PAGE_SIZE_MASK;
    page_index = phy_addr / PAGE_SIZE;
    
    if ((page_index >= phymm_valid) && (page_index < phymm_max))
    {
        if (phymm_dereference_page(phy_addr / PAGE_SIZE) == 0)
            phymm_free_user(phy_addr / PAGE_SIZE);
        mm_clear_page_table_entry(dir_entry, page_table, table_entry);
    }
}

unsigned mm_get_map_flag(unsigned vir)
{
    unsigned int *table_entry;
    unsigned flag;
    if (!mm_get_valid_page_table(vir, 0, 
        NULL, NULL, NULL,
        &table_entry))
        return 0;
    flag = *table_entry;
    flag &= ~PAGE_SIZE_MASK;
    return flag;
}

void mm_set_map_flag(unsigned vir, unsigned flag)
{
    unsigned int *table_entry;
    unsigned f;
    if (!mm_get_valid_page_table(vir, 0, 
        NULL, NULL, NULL,
        &table_entry))
        return 0;
    f = *table_entry;
    f &= PAGE_SIZE_MASK;
    f |= flag;
    *table_entry = f;
}

unsigned mm_get_attached_page_index(unsigned int vir)
{ 
    unsigned int phy_addr;
    unsigned int *table_entry;
    if (!mm_get_valid_page_table(vir, 0, 
        NULL, NULL, NULL,
        &table_entry))
        return 0;
    phy_addr = (*table_entry) & PAGE_SIZE_MASK;
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


// path cache
static void lock_path_cache()
{
    spinlock_lock(&path_lock);
} 

static void unlock_path_cache()
{
    spinlock_unlock(&path_lock);
}



void* name_get()
{
    int pc = MAX_PATH / PAGE_SIZE;
    void* buf = NULL;
    name_cache* node = NULL;
    lock_path_cache();

    if (IsListEmpty(&name_cache_head)){
        buf = vm_alloc(pc);
    }else{
        node = RemoveHeadList(&name_cache_head);
        buf = node->buf;
        free(node);
    }

    unlock_path_cache();
    return buf;
}

void name_put(void* buf)
{
    name_cache* node = NULL;
    lock_path_cache();
    node = calloc(1, sizeof(*node));
    node->buf = buf;
    InsertHeadList(&name_cache_head, &node->list);
    unlock_path_cache();
}