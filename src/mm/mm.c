#include <mm.h>
#include <klib.h>
#include <multiboot.h>
#include <list.h>
#include <lock.h>
#include <ps.h>
#include <time.h>
#include <errno.h>
#include <mmap.h>
#include <phymm.h>
#include <macro.h>

static void mm_clear_beginning_user_map();

short pgc_entry_count[1024];
unsigned phymm_end = 0;
unsigned phymm_begin = 0;

unsigned mm_get_pagedir()
{
	unsigned cr3 = 0;
	LOAD_CR3(cr3);
	return cr3 + KERNEL_OFFSET;
}

typedef struct _page_table_cache {
	unsigned int top;
	unsigned int count;
	unsigned int mem[1024];
} page_table_cache_t;

unsigned pgc_count;
unsigned pgc_top;

static void page_table_cache_init(page_table_cache_t *cache)
{
	int i = 0;

	for (i = 0; i < 1024; i++) {
		cache->mem[i] = PAGE_TABLE_CACHE_BEGIN + i * PAGE_SIZE;
	}
	cache->top = 1023;
	cache->count = 1024;
}

static int page_table_cache_free(page_table_cache_t *cache, unsigned int val)
{
	if (cache->count < 1024) {
		cache->mem[cache->top] = val;
		__sync_add_and_fetch(&(cache->top), 1);
		__sync_add_and_fetch(&(cache->count), 1);
		pgc_count = cache->count;
		pgc_top = cache->top;
		return 1;
	} else {
		return 0;
	}
}

static unsigned int page_table_cache_alloc(page_table_cache_t *cache)
{
	unsigned ret = 0;
	if (cache->count > 0) {
		__sync_add_and_fetch(&(cache->top), -1);
		ret = cache->mem[cache->top];
		__sync_add_and_fetch(&(cache->count), -1);
		pgc_count = cache->count;
		pgc_top = cache->top;
	} else {
		ret = 0;
	}

	return ret;
}

page_table_cache_t page_table_cache;

typedef struct _name_cache {
	list_entry list;
	void *buf;
} name_cache;

static name_cache name_cache_head;
static spinlock_t mm_lock;
static spinlock_t path_lock;

// take 8M ~ 12M as page table cache
void mm_init_page_table_cache()
{
	int i = 0;

	page_table_cache_init(&page_table_cache);

	for (i = 0; i < 1024; i++) {
		pgc_entry_count[i] = 0;
	}

	spinlock_init(&mm_lock);
	spinlock_init(&path_lock);
	list_init(&name_cache_head);
}

static void lock_mm()
{
	spinlock_lock(&mm_lock);
}

static void unlock_mm()
{
	spinlock_unlock(&mm_lock);
}

static void mm_clear_beginning_user_map()
{
	unsigned int reserved_page_tables = (RESERVED_PAGES / 1024);
	unsigned int *gdt = (int *)(GDT_ADDRESS + KERNEL_OFFSET);
	int i;

	for (i = 0; i < reserved_page_tables; i++) {
		gdt[i] = 0;
	}
}

typedef struct _mm_addr_info {
	unsigned int *dir;
	unsigned int *table;
	unsigned int *entry;
} mm_addr_info;

static int mm_get_valid_page_table(unsigned addr, unsigned flag,
				   mm_addr_info *info)
{
	unsigned int *_page_dir;
	unsigned offset = ADDR_TO_PGT_OFFSET(addr);

	_page_dir = (unsigned int *)mm_get_pagedir();

	if ((_page_dir[offset] & PAGE_SIZE_MASK) == 0) {
		unsigned int table_addr = mm_alloc_page_table();
		if (table_addr == 0) {
			return 0;
		}
		_page_dir[offset] = (table_addr - KERNEL_OFFSET) |
				    PAGE_ENTRY_KERNEL_DATA;
		_page_dir[offset] |= flag;
	}
	info->dir = &(_page_dir[offset]);
	info->table =
		(unsigned int *)((*info->dir & PAGE_SIZE_MASK) + KERNEL_OFFSET);
	info->entry = &(info->table[ADDR_TO_PET_OFFSET(addr)]);

	return 1;
}

static int mm_set_page_table_entry(unsigned addr, unsigned flag, unsigned value)
{
	mm_addr_info info;
	unsigned int data;

	if (!mm_get_valid_page_table(addr, flag, &info))
		return 0;

	data = (*info.entry) & PAGE_SIZE_MASK;
	if (!data) {
		int idx = (PAGE_TABLE_CACHE_END - (unsigned)info.table) /
				  PAGE_SIZE -
			  1;
		pgc_entry_count[idx]++;
	}

	(*info.entry) = value;
	return 1;
}

static int mm_clear_page_table_entry(mm_addr_info *info)
{
	unsigned phy;
	int idx, empty;
	phy = (*(info->entry)) & PAGE_SIZE_MASK;
	*(info->entry) = 0;
	if (phy) {
		idx = (PAGE_TABLE_CACHE_END - (unsigned)info->table) /
			      PAGE_SIZE -
		      1;
		pgc_entry_count[idx]--;
	}

	if (pgc_entry_count[idx] == 0) {
		mm_free_page_table((unsigned int)info->table);
		*(info->dir) = 0;
	}
}

int mm_add_resource_map(unsigned int phy)
{
	mm_addr_info info;
	unsigned int phy_page = (phy)&PAGE_SIZE_MASK;

	if (phy < KERNEL_OFFSET)
		return -1;

	if (!mm_get_valid_page_table(phy, 0, &info))
		return -1;

	*info.entry = phy_page | PAGE_ENTRY_KERNEL_DATA;
	return 1;
}

int mm_add_direct_map(unsigned int vir)
{
	unsigned int phy_page = (vir - KERNEL_OFFSET) & PAGE_SIZE_MASK;
	unsigned int page_busy = 0;
	unsigned int page_index = 0;
	// only kernel space has direct map
	if (vir < KERNEL_OFFSET)
		return -1;

	if (vir >= KERNEL_OFFSET && vir < PAGE_TABLE_CACHE_END)
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
	mm_addr_info info;
	unsigned int phy;
	unsigned int phy_addr = 0;
	int empty = 1;
	int i = 0, idx;

	// 0xC0000000 ~ PAGE_TABLE_CACHE_END is always mapped
	if (vir >= KERNEL_OFFSET && vir < PAGE_TABLE_CACHE_END)
		return;

	phy_addr = vir & PAGE_SIZE_MASK;
	if (vir >= KERNEL_OFFSET) {
		phy_addr -= KERNEL_OFFSET;
	}

	if (!mm_get_valid_page_table(vir, 0, &info))
		return;

	if (!mm_clear_page_table_entry(&info))
		return;

	phymm_dereference_page(phy_addr / PAGE_SIZE);
}

void mm_del_user_map()
{
	unsigned int *page_dir = (unsigned int *)mm_get_pagedir();
	page_dir[0] = 0;
	page_dir[1] = 0;
}

unsigned int mm_alloc_page_table()
{
	unsigned int ret = page_table_cache_alloc(&page_table_cache);
	task_struct *cur = CURRENT_TASK();
	if (ret == 0)
		return ret;
	memset((void *)ret, 0, PAGE_SIZE);
	return ret;
}

void mm_free_page_table(unsigned int vir)
{
	page_table_cache_free(&page_table_cache, vir);
}

unsigned int vm_alloc(int page_count)
{
	int page_index = 0;
	int i = 0;
	int valid = 0;
	unsigned int vir = 0;

	lock_mm();

	page_index = phymm_alloc_kernel(page_count);
	if (page_index < 0) {
		return NULL;
	}

	for (i = 0; i < page_count; i++) {
		vir = (page_index + i) * PAGE_SIZE + KERNEL_OFFSET;
		mm_add_direct_map(vir);
	}

	RELOAD_CR3();
	vir = page_index * PAGE_SIZE + KERNEL_OFFSET;
	// memset(vir, 0, page_count * PAGE_SIZE);
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
	for (i = 0; i < page_count; i++) {
		cur = vm + i * PAGE_SIZE;
		phy = cur - KERNEL_OFFSET;
		mm_del_direct_map(cur);
	}
	RELOAD_CR3();
	phymm_free_kernel((vm - KERNEL_OFFSET) / PAGE_SIZE, page_count);

	unlock_mm();
}

int mm_add_dynamic_map(unsigned int vir, unsigned int phy, unsigned flag)
{
	unsigned int phy_page;
	unsigned int *page_dir, *table, *entry;
	unsigned int page_busy = 0;
	unsigned int page_index = 0;
	unsigned int target_phy = 0;

	if (phy) {
		phy = phy & PAGE_SIZE_MASK;
		page_index = phy / PAGE_SIZE;
		target_phy = phy;
	} else {
		page_index = phymm_alloc_user();
		target_phy = page_index * PAGE_SIZE;
	}

	phy_page = target_phy & PAGE_SIZE_MASK;

	if (!mm_set_page_table_entry(vir, (flag | PAGE_ENTRY_WRITABLE),
				     phy_page | flag))
		return -1;

	phymm_reference_page(phy_page / PAGE_SIZE);

	return 1;
}

void mm_del_dynamic_map(unsigned int vir)
{
	mm_addr_info info;
	unsigned int phy_addr;
	int empty = 1;
	int i = 0, idx;
	int page_index;

	if (!mm_get_valid_page_table(vir, 0, &info))
		return;

	phy_addr = (*info.entry) & PAGE_SIZE_MASK;
	page_index = phy_addr / PAGE_SIZE;

	if ((page_index >= phymm_begin) && (page_index < phymm_end)) {
		if (phymm_dereference_page(phy_addr / PAGE_SIZE) == 0) {
			phymm_free_user(phy_addr / PAGE_SIZE);
		}

		mm_clear_page_table_entry(&info);
	}
}

unsigned mm_get_map_flag(unsigned vir)
{
	mm_addr_info info;
	unsigned flag;
	if (!mm_get_valid_page_table(vir, 0, &info))
		return 0;
	flag = *info.entry;
	flag &= ~PAGE_SIZE_MASK;
	return flag;
}

void mm_set_map_flag(unsigned vir, unsigned flag)
{
	mm_addr_info info;
	unsigned f;
	if (!mm_get_valid_page_table(vir, 0, &info))
		return;
	f = *info.entry;
	f &= PAGE_SIZE_MASK;
	f |= flag;
	*info.entry = f;
}

unsigned mm_get_attached_page_index(unsigned int vir)
{
	unsigned int phy_addr;
	mm_addr_info info;
	if (!mm_get_valid_page_table(vir, 0, &info))
		return 0;
	phy_addr = (*info.entry) & PAGE_SIZE_MASK;
	return (phy_addr / PAGE_SIZE);
}

unsigned vm_get_usr_zone(unsigned page_count)
{
	// FIXME
	// only used in load elf, please modify it
	task_struct *cur = CURRENT_TASK();
	unsigned begin = vm_disc_map(cur->user.vm, page_count * PAGE_SIZE);

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

void *name_get()
{
	int pc = MAX_PATH / PAGE_SIZE;
	void *buf = NULL;
	name_cache *node = NULL;
	lock_path_cache();

	if (list_is_empty(&name_cache_head)) {
		buf = (void *)vm_alloc(pc);
	} else {
		node = list_remove_head(&name_cache_head);
		buf = node->buf;
		free(node);
	}

	unlock_path_cache();
	return buf;
}

void name_put(void *buf)
{
	name_cache *node = NULL;
	lock_path_cache();
	node = calloc(1, sizeof(*node));
	node->buf = buf;
	list_insert_head(&name_cache_head, &node->list);
	unlock_path_cache();
}