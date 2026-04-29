#include "config.h"
#include <boot/multiboot.h>
#include <lib/klib.h>
#include <lib/list.h>
#include <lib/lock.h>
#include <ps/ps.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/vdso.h>
#include <macro.h>

/* Physical memory range tracked by the page allocator */
unsigned phymm_end = 0;
unsigned phymm_begin = 0;

unsigned cache_count = 0;
unsigned buffer_count = 0;
unsigned pgc_top = 0;

/*
 * Page table cache
 *
 * The region PAGE_TABLE_CACHE_BEGIN..PAGE_TABLE_CACHE_END (8 MB – 12 MB) is
 * statically reserved for page tables.  A simple stack-based cache dishes out
 * and reclaims 4 KB entries from that region.
 */

/* Live entry counts per cached page table (used to reclaim empty tables) */
short pgc_entry_count[PAGE_TABLE_CACHE_PAGES];

typedef struct {
	unsigned int top;
	unsigned int count;
	unsigned int mem[PAGE_TABLE_CACHE_PAGES];
} page_table_cache_t;

static page_table_cache_t page_table_cache;
static unsigned int kernel_pde_tables[1024 - KERNEL_PAGE_DIR_OFFSET];

#define KERNEL_KMAP_PAGES ((KERNEL_KMAP_END - KERNEL_KMAP_BEGIN) / PAGE_SIZE)

static unsigned int kernel_kmap_phys[KERNEL_KMAP_PAGES];

static void page_table_cache_init(page_table_cache_t *cache)
{
	int i;

	for (i = 0; i < PAGE_TABLE_CACHE_PAGES; i++)
		cache->mem[i] = PAGE_TABLE_CACHE_BEGIN + i * PAGE_SIZE;
	cache->top = PAGE_TABLE_CACHE_PAGES - 1;
	cache->count = PAGE_TABLE_CACHE_PAGES;
}

static unsigned int page_table_cache_alloc(page_table_cache_t *cache)
{
	unsigned int ret;

	if (cache->count == 0)
		return 0;
	ret = cache->mem[cache->top];
	__sync_add_and_fetch(&cache->top, -1);
	pgc_top = __sync_add_and_fetch(&cache->count, -1);
	cache_count++;
	return ret;
}

static void page_table_cache_free(page_table_cache_t *cache, unsigned int val)
{
	if (cache->count >= PAGE_TABLE_CACHE_PAGES)
		return;
	__sync_add_and_fetch(&cache->top, 1);
	cache->mem[cache->top] = val;
	pgc_top = __sync_add_and_fetch(&cache->count, 1);
	cache_count--;
}

unsigned int mm_alloc_page_table()
{
	unsigned int ret = page_table_cache_alloc(&page_table_cache);

	if (ret == 0)
		return 0;
	memset((void *)ret, 0, PAGE_SIZE);
	return ret;
}

void mm_free_page_table(unsigned int vir)
{
	page_table_cache_free(&page_table_cache, vir);
}

static void mm_init_kernel_page_dir_template(void)
{
	unsigned int *page_dir = (unsigned int *)mm_get_pagedir();
	unsigned int i;

	for (i = 0; i < 1024 - KERNEL_PAGE_DIR_OFFSET; i++) {
		if (page_dir[KERNEL_PAGE_DIR_OFFSET + i] & PAGE_ENTRY_PRESENT)
			continue;

		unsigned int table_addr =
			page_table_cache_alloc(&page_table_cache);

		if (table_addr == 0)
			break;

		memset((void *)table_addr, 0, PAGE_SIZE);
		kernel_pde_tables[i] = table_addr;
		page_dir[KERNEL_PAGE_DIR_OFFSET + i] = VIRT_TO_PHY(table_addr) |
						       PAGE_ENTRY_KERNEL_DATA;
	}
}

/*
 * Locks and initialisation
 */

static spinlock_t mm_lock;
static spinlock_t path_lock;
static spinlock_t kmap_lock;
static int mm_dynamic_region(unsigned phy);

/* Name-buffer cache node */

static list_entry name_cache_head;

/* Called once at boot: set up the page-table cache and related state */
void mm_init_page_table_cache()
{
	int i;

	page_table_cache_init(&page_table_cache);
	memset(kernel_pde_tables, 0, sizeof(kernel_pde_tables));
	mm_init_kernel_page_dir_template();
	for (i = 0; i < PAGE_TABLE_CACHE_PAGES; i++)
		pgc_entry_count[i] = 0;

	spinlock_init(&mm_lock);
	spinlock_init(&path_lock);
	spinlock_init(&kmap_lock);
	list_init(&name_cache_head);
}

void mm_init_process_page_dir(unsigned int page_dir)
{
	unsigned int *dst = (unsigned int *)page_dir;
	unsigned int *src = (unsigned int *)mm_get_pagedir();

	memset(dst, 0, PAGE_SIZE);
	memcpy(&dst[KERNEL_PAGE_DIR_OFFSET], &src[KERNEL_PAGE_DIR_OFFSET],
	       (1024 - KERNEL_PAGE_DIR_OFFSET) * sizeof(unsigned int));
}

/*
 * Internal: page directory / page table helpers
 */

unsigned mm_get_pagedir()
{
	unsigned cr3 = 0;

	LOAD_CR3(cr3);
	return cr3 + KERNEL_OFFSET;
}

unsigned int mm_phys_to_virt(unsigned int phys)
{
	unsigned int page = phys & PAGE_SIZE_MASK;
	unsigned int off = ADDR_TO_PAGE_OFFSET(phys);
	unsigned int i;

	if (page < KERNEL_DIRECT_MAP_LIMIT)
		return KERNEL_OFFSET + page + off;

	for (i = 0; i < KERNEL_KMAP_PAGES; i++) {
		if (kernel_kmap_phys[i] == page + 1)
			return KERNEL_KMAP_BEGIN + i * PAGE_SIZE + off;
	}

	if (mm_kmap_phys(page) == 1) {
		for (i = 0; i < KERNEL_KMAP_PAGES; i++) {
			if (kernel_kmap_phys[i] == page + 1)
				return KERNEL_KMAP_BEGIN + i * PAGE_SIZE + off;
		}
	}

	return 0;
}

typedef struct {
	unsigned int *dir; /* page-directory entry for this address */
	unsigned int *table; /* base of the page table */
	unsigned int *entry; /* page-table entry for this address */
} mm_addr_info;

static int mm_get_valid_page_table_in_dir(unsigned int *page_dir, unsigned addr,
					  mm_addr_info *info)
{
	unsigned offset = ADDR_TO_PGT_OFFSET(addr);

	info->dir = info->table = info->entry = NULL;
	if (!page_dir)
		return 0;

	info->dir = &page_dir[offset];
	if ((*info->dir & PAGE_SIZE_MASK) == 0)
		return 0;

	info->table = (unsigned int *)PHY_TO_VIRT(*info->dir & PAGE_SIZE_MASK);
	info->entry = &info->table[ADDR_TO_PET_OFFSET(addr)];
	return 1;
}

/*
 * Locate (and optionally allocate) the page-table entry for @addr.
 * Returns 1 on success; 0 if a new page table was needed but allocation failed.
 */
static int mm_get_valid_page_table(unsigned addr, unsigned flag,
				   mm_addr_info *info, int alloc_if_none)
{
	unsigned int *page_dir = (unsigned int *)mm_get_pagedir();
	unsigned offset = ADDR_TO_PGT_OFFSET(addr);

	info->dir = info->table = info->entry = NULL;

	if ((page_dir[offset] & PAGE_SIZE_MASK) == 0) {
		if (!alloc_if_none)
			return 0;

		unsigned int table_addr = mm_alloc_page_table();
		unsigned int pde;

		if (table_addr == 0)
			return 0;
		pde = VIRT_TO_PHY(table_addr) | PAGE_ENTRY_KERNEL_DATA | flag;
		page_dir[offset] = pde;
	}
	info->dir = &page_dir[offset];
	if (*info->dir)
		info->table = (unsigned int *)PHY_TO_VIRT(*info->dir &
							  PAGE_SIZE_MASK);
	if (info->table)
		info->entry = &info->table[ADDR_TO_PET_OFFSET(addr)];

	return info->entry != NULL;
}

unsigned int mm_virt_to_phys(unsigned int virt)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(virt, 0, &info, 0) ||
	    !(*info.entry & PAGE_ENTRY_PRESENT)) {
		return 0;
	}
	return (*info.entry & PAGE_SIZE_MASK) + ADDR_TO_PAGE_OFFSET(virt);
}

/*
 * Write @value into the page-table entry for @addr, allocating a page table if
 * necessary.  Increments the per-table live-entry counter.
 * Returns 1 on success, 0 on allocation failure.
 */
static int mm_set_page_table_entry(unsigned addr, unsigned flag, unsigned value)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(addr, flag, &info, 1))
		return 0;

	/* Track live entries so we know when to reclaim the page table */
	if (addr < KERNEL_OFFSET && (*info.entry & PAGE_SIZE_MASK) == 0) {
		int idx = (PAGE_TABLE_CACHE_END - (unsigned)info.table) /
				  PAGE_SIZE -
			  1;
		pgc_entry_count[idx]++;
	}

	*info.entry = value;
	return 1;
}

/*
 * Zero the page-table entry and decrement the live-entry counter.
 * Reclaims the page table itself when the last entry is removed.
 */
static void mm_clear_page_table_entry(mm_addr_info *info)
{
	unsigned phy = *info->entry & PAGE_SIZE_MASK;

	*info->entry = 0;
	if (phy) {
		unsigned dir_index =
			(unsigned)(info->dir -
				   (unsigned int *)mm_get_pagedir());

		if (dir_index < KERNEL_PAGE_DIR_OFFSET) {
			int idx =
				(PAGE_TABLE_CACHE_END - (unsigned)info->table) /
					PAGE_SIZE -
				1;
			pgc_entry_count[idx]--;
			if (pgc_entry_count[idx] == 0) {
				mm_free_page_table((unsigned int)info->table);
				*info->dir = 0;
			}
		}
	}
}

/*
 * Map management
 */

/*
 * Add a low-memory direct mapping for a kernel virtual address.
 * This is used during boot for reserved kernel pages and for the physical
 * memory descriptor array.  It does not change physical allocator refcounts.
 */
int mm_kmap_page(unsigned int vir)
{
	unsigned int page_index;

	if (vir < KERNEL_OFFSET)
		return -1;
	if (vir >= KERNEL_KMAP_BEGIN)
		return -1;

	/* The page-table cache region is permanently mapped */
	if (vir < PAGE_TABLE_CACHE_END)
		return 1;

	page_index = (vir - KERNEL_OFFSET) / PAGE_SIZE;

	if (!mm_set_page_table_entry(
		    vir, 0, (page_index * PAGE_SIZE) | PAGE_ENTRY_KERNEL_DATA))
		return -1;

	return 1;
}

/* Remove a kernel mapping without touching physical allocator refcounts. */
void mm_kunmap_page(unsigned int vir)
{
	mm_addr_info info;

	/* The page-table cache region is permanently mapped */
	if (vir >= KERNEL_OFFSET && vir < PAGE_TABLE_CACHE_END)
		return;

	if (!mm_get_valid_page_table(vir, 0, &info, 0))
		return;

	mm_clear_page_table_entry(&info);
}

/*
 * Map a physical page into the kernel kmap alias window.
 *
 * Works for any physical address and does not touch physical allocator
 * reference counts.  Firmware pages and user pages may both pass through this
 * path.
 *
 * Returns -1 on page-table allocation failure or alias-space exhaustion.
 */
int mm_kmap_phys(unsigned int phys)
{
	unsigned int page = phys & PAGE_SIZE_MASK;
	unsigned int slot;
	unsigned int virt;
	int irq;
	mm_addr_info info;

	if (page < KERNEL_DIRECT_MAP_LIMIT) {
		virt = KERNEL_OFFSET + page;
		if (!mm_get_valid_page_table(virt, 0, &info, 1))
			return -1;
		*info.entry = page | PAGE_ENTRY_KERNEL_DATA;
		INVLPG(virt);
		return 1;
	}

	spinlock_lock(&kmap_lock, &irq);
	for (slot = 0; slot < KERNEL_KMAP_PAGES; slot++) {
		if (kernel_kmap_phys[slot] == page + 1) {
			spinlock_unlock(&kmap_lock, irq);
			return 1;
		}
	}

	for (slot = 0; slot < KERNEL_KMAP_PAGES; slot++) {
		if (kernel_kmap_phys[slot] == 0)
			break;
	}
	if (slot == KERNEL_KMAP_PAGES) {
		spinlock_unlock(&kmap_lock, irq);
		return -1;
	}

	kernel_kmap_phys[slot] = page + 1;
	spinlock_unlock(&kmap_lock, irq);

	phys = KERNEL_KMAP_BEGIN + slot * PAGE_SIZE;
	virt = phys;
	if (!mm_get_valid_page_table(virt, 0, &info, 1)) {
		spinlock_lock(&kmap_lock, &irq);
		kernel_kmap_phys[slot] = 0;
		spinlock_unlock(&kmap_lock, irq);
		return -1;
	}

	*info.entry = page | PAGE_ENTRY_KERNEL_DATA;
	INVLPG(virt);
	return 1;
}

/*
 * Map a high physical address (e.g. MMIO resource) into the kernel address
 * space at the same virtual address.
 */
int mm_map_io(unsigned int phy)
{
	mm_addr_info info;

	if (phy < KERNEL_OFFSET)
		return -1;

	if (!mm_get_valid_page_table(phy, 0, &info, 1))
		return -1;

	*info.entry = (phy & PAGE_SIZE_MASK) | PAGE_ENTRY_KERNEL_DATA;
	return 1;
}

/* Remove the temporary low identity map installed during boot. */
void mm_del_user_map()
{
	unsigned int *page_dir = (unsigned int *)mm_get_pagedir();
	unsigned int reserved_page_tables =
		(RESERVED_PAGES + PE_TABLE_SIZE - 1) / PE_TABLE_SIZE;
	unsigned int i;

	for (i = 0; i < reserved_page_tables; i++)
		page_dir[i] = 0;
	RELOAD_CR3();
}

/*
 * Destroy every user-space mapping in @page_dir in one pass.
 *
 * This is faster than repeated mm_unmap_page() calls during exit/exec because
 * it walks each user page table exactly once, drops all backing user pages,
 * then frees the page table as a whole.
 */
void mm_destroy_user_map(unsigned int page_dir)
{
	unsigned int *dir = (unsigned int *)page_dir;
	int irq;
	unsigned int i;

	if (!dir)
		return;

	spinlock_lock(&mm_lock, &irq);
	for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
		unsigned int *table;
		unsigned int table_phy;
		unsigned int j;
		int cache_idx;

		table_phy = dir[i] & PAGE_SIZE_MASK;
		if (!table_phy)
			continue;

		table = (unsigned int *)PHY_TO_VIRT(table_phy);
		for (j = 0; j < PG_TABLE_SIZE; j++) {
			unsigned int phy_addr = table[j] & PAGE_SIZE_MASK;
			unsigned int page_index;

			if (!phy_addr)
				continue;

			page_index = PHY_TO_PAGE_IDX(phy_addr);
			if ((mm_dynamic_region(phy_addr) ||
			     mm_vdso_region(phy_addr)) &&
			    phymm_is_used(page_index) &&
			    phymm_dereference_page(page_index) == 0)
				phymm_free_user(page_index);

			table[j] = 0;
		}

		cache_idx =
			(PAGE_TABLE_CACHE_END - (unsigned)table) / PAGE_SIZE -
			1;
		pgc_entry_count[cache_idx] = 0;
		mm_free_page_table((unsigned int)table);
		dir[i] = 0;
	}
	spinlock_unlock(&mm_lock, irq);
}

/*
 * Map @vir to a physical page.  If @phy is 0 a fresh user page is allocated;
 * otherwise the caller-supplied physical address is used.
 * Returns 1 on success, -1 on failure.
 */
int mm_map_page(unsigned int vir, unsigned int phy, unsigned flag)
{
	unsigned int target_phy;
	unsigned int page_index;
	int irq;

	spinlock_lock(&mm_lock, &irq);

	if (phy) {
		page_index = (phy & PAGE_SIZE_MASK) / PAGE_SIZE;
		target_phy = phy & PAGE_SIZE_MASK;
	} else {
		page_index = phymm_alloc_user();
		if (page_index == PHYMM_INVALID) {
			spinlock_unlock(&mm_lock, irq);
			return -1;
		}
		target_phy = page_index * PAGE_SIZE;
	}

	if (!mm_set_page_table_entry(vir, flag | PAGE_ENTRY_WRITABLE,
				     target_phy | flag)) {
		spinlock_unlock(&mm_lock, irq);
		return -1;
	}

	phymm_reference_page(page_index);
	spinlock_unlock(&mm_lock, irq);
	return 1;
}

/*
 * Map @vir directly to an MMIO/physical page without touching the physical
 * allocator reference counts.  This is for /dev/mem-style mappings of device
 * BARs or firmware regions, not normal RAM-backed user pages.
 */
int mm_map_page_io(unsigned int vir, unsigned int phy, unsigned flag)
{
	int irq;

	spinlock_lock(&mm_lock, &irq);
	if (!mm_set_page_table_entry(vir, flag,
				     (phy & PAGE_SIZE_MASK) | flag)) {
		spinlock_unlock(&mm_lock, irq);
		return -1;
	}
	spinlock_unlock(&mm_lock, irq);
	return 1;
}

static int mm_dynamic_region(unsigned phy)
{
	unsigned begin = phymm_begin * PAGE_SIZE;
	unsigned end = phymm_end * PAGE_SIZE;
	return phy >= begin && phy < end;
}

/* Remove a dynamic user mapping and free the physical page if unreferenced */
void mm_unmap_page(unsigned int vir)
{
	mm_addr_info info;
	unsigned int phy_addr;
	int page_index;
	int irq;

	if (!mm_get_valid_page_table(vir, 0, &info, 0))
		return;

	phy_addr = *info.entry & PAGE_SIZE_MASK;
	page_index = PHY_TO_PAGE_IDX(phy_addr);

	spinlock_lock(&mm_lock, &irq);
	if (mm_dynamic_region(phy_addr) || mm_vdso_region(phy_addr)) {
		if (phymm_is_used(page_index) &&
		    phymm_dereference_page(page_index) == 0)
			phymm_free_user(page_index);
	}
	mm_clear_page_table_entry(&info);
	spinlock_unlock(&mm_lock, irq);
}

/* Return the page-table flags (low 12 bits) for the mapping at @vir */
unsigned mm_get_map_flag(unsigned vir)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(vir, 0, &info, 0))
		return 0;
	return *info.entry & ~PAGE_SIZE_MASK;
}

unsigned mm_get_map_flag_pd(unsigned page_dir, unsigned vir)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table_in_dir((unsigned int *)page_dir, vir,
					    &info))
		return 0;
	return *info.entry & ~PAGE_SIZE_MASK;
}

/* Update the page-table flags for the mapping at @vir */
void mm_set_map_flag(unsigned vir, unsigned flag)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(vir, 0, &info, 0))
		return;
	*info.entry = (*info.entry & PAGE_SIZE_MASK) | flag;
}

void mm_set_map_flag_pd(unsigned page_dir, unsigned vir, unsigned flag)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table_in_dir((unsigned int *)page_dir, vir,
					    &info))
		return;
	*info.entry = (*info.entry & PAGE_SIZE_MASK) | flag;
}

/* Return the physical page index backing the virtual address @vir */
unsigned mm_get_attached_page_index(unsigned int vir)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(vir, 0, &info, 0))
		return 0;
	return (*info.entry & PAGE_SIZE_MASK) / PAGE_SIZE;
}

/*
 * Virtual memory allocator (kernel heap)
 */

/* Allocate @page_count contiguous kernel pages; returns virtual base address */
unsigned int vm_alloc(int page_count)
{
	int page_index;
	int i;
	int irq;
	unsigned int vm;

	spinlock_lock(&mm_lock, &irq);

	page_index = phymm_alloc_kernel(page_count);
	if (page_index < 0) {
		spinlock_unlock(&mm_lock, irq);
		return 0;
	}

	vm = KERNEL_OFFSET + page_index * PAGE_SIZE;
	for (i = 0; i < page_count; i++) {
		if (mm_kmap_page(vm + i * PAGE_SIZE) != 1) {
			while (i-- > 0) {
				phymm_dereference_page(page_index + i);
				mm_kunmap_page(vm + i * PAGE_SIZE);
			}
			phymm_free_kernel(page_index, page_count);
			spinlock_unlock(&mm_lock, irq);
			return 0;
		}
		phymm_reference_page(page_index + i);
	}

	RELOAD_CR3();
	spinlock_unlock(&mm_lock, irq);

	buffer_count += page_count;
	return vm;
}

/* Release @page_count pages starting at kernel virtual address @vm */
void vm_free(unsigned int vm, int page_count)
{
	int i;
	int irq;
	unsigned int page_index;

	spinlock_lock(&mm_lock, &irq);
	vm &= PAGE_SIZE_MASK;
	page_index = VIRT_TO_PAGE_IDX(vm);
	for (i = 0; i < page_count; i++) {
		unsigned int phy = VIRT_TO_PHY(vm + i * PAGE_SIZE);

		mm_kunmap_page(vm + i * PAGE_SIZE);
		if (phy)
			phymm_dereference_page(PHY_TO_PAGE_IDX(phy));
	}
	RELOAD_CR3();
	phymm_free_kernel(page_index, page_count);
	spinlock_unlock(&mm_lock, irq);

	buffer_count -= page_count;
}

/*
 * Name / path buffer cache
 *
 * Caches MAX_PATH-sized buffers to avoid repeated vm_alloc/vm_free calls for
 * temporary pathname storage.
 */

/* Acquire a pathname buffer (allocates a new one if the cache is empty) */
void *name_get()
{
	char *buf, *region;
	int irq;

	spinlock_lock(&path_lock, &irq);
	if (list_is_empty(&name_cache_head)) {
		spinlock_unlock(&path_lock, irq);
		region = (void *)vm_alloc(1);
		buf = region + sizeof(list_entry);
	} else {
		region = (char *)list_remove_tail(&name_cache_head);
		spinlock_unlock(&path_lock, irq);
		buf = region + sizeof(list_entry);
	}
	memset(buf, 0, MAX_PATH);
	cache_count++;
	return buf;
}

/* Return a pathname buffer to the cache */
void name_put(void *buf)
{
	list_entry *region;
	int irq;

	spinlock_lock(&path_lock, &irq);
	region = (list_entry *)((char *)buf - sizeof(list_entry));
	list_insert_tail(&name_cache_head, region);
	spinlock_unlock(&path_lock, irq);
	cache_count--;
}
