#include <mm.h>
#include <klib.h>
#include <multiboot.h>
#include <list.h>
#include <lock.h>
#include <ps.h>
#include <mmap.h>
#include <phymm.h>
#include <macro.h>

/* Physical memory range tracked by the page allocator */
unsigned phymm_end = 0;
unsigned phymm_begin = 0;

/* ---------------------------------------------------------------------------
 * Global kernel page directory
 *
 * Entries KERNEL_PAGE_DIR_OFFSET..1023 (the kernel half of the page directory)
 * are the same for every task.  Instead of copying them on every context
 * switch, we maintain a global canonical copy here.
 *
 * - mm_init_page_table_cache() seeds this from the boot page directory.
 * - mm_get_valid_page_table() updates it whenever a new kernel PDE is
 *   allocated, then fires kernel_pde_propagator so ps.c can push the entry
 *   to all existing task page directories (rare: only on first access of a
 *   new 4 MB kernel region).
 * - New task page directories call mm_copy_kernel_pgd() at creation time
 *   and are immediately up-to-date.  No per-switch sync is ever needed.
 * ---------------------------------------------------------------------------*/
#define KERNEL_PGD_ENTRIES (1024 - KERNEL_PAGE_DIR_OFFSET)

static unsigned int kernel_pgd[KERNEL_PGD_ENTRIES];
static kernel_pde_propagator_t kernel_pde_propagator;

void mm_set_kernel_pde_propagator(kernel_pde_propagator_t fn)
{
	kernel_pde_propagator = fn;
}

void mm_copy_kernel_pgd(unsigned int *pgd)
{
	memcpy(&pgd[KERNEL_PAGE_DIR_OFFSET], kernel_pgd,
	       KERNEL_PGD_ENTRIES * sizeof(unsigned int));
}
unsigned pgc_count = 0;
unsigned pgc_top = 0;

/* ---------------------------------------------------------------------------
 * Page table cache
 *
 * The region PAGE_TABLE_CACHE_BEGIN..PAGE_TABLE_CACHE_END (8 MB – 12 MB) is
 * statically reserved for page tables.  A simple stack-based cache dishes out
 * and reclaims 4 KB entries from that region.
 * ---------------------------------------------------------------------------*/

/* Live entry counts per cached page table (used to reclaim empty tables) */
short pgc_entry_count[1024];

typedef struct {
	unsigned int top;
	unsigned int count;
	unsigned int mem[1024];
} page_table_cache_t;

static page_table_cache_t page_table_cache;

static void page_table_cache_init(page_table_cache_t *cache)
{
	int i;

	for (i = 0; i < 1024; i++)
		cache->mem[i] = PAGE_TABLE_CACHE_BEGIN + i * PAGE_SIZE;
	cache->top = 1023;
	cache->count = 1024;
}

static unsigned int page_table_cache_alloc(page_table_cache_t *cache)
{
	unsigned int ret;

	if (cache->count == 0)
		return 0;
	pgc_top = __sync_add_and_fetch(&cache->top, -1);
	ret = cache->mem[cache->top];
	pgc_count = __sync_add_and_fetch(&cache->count, -1);
	return ret;
}

static void page_table_cache_free(page_table_cache_t *cache, unsigned int val)
{
	if (cache->count >= 1024)
		return;
	cache->mem[cache->top] = val;
	pgc_top = __sync_add_and_fetch(&cache->top, 1);
	pgc_count = __sync_add_and_fetch(&cache->count, 1);
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

/* ---------------------------------------------------------------------------
 * Locks and initialisation
 * ---------------------------------------------------------------------------*/

static spinlock_t mm_lock;
static spinlock_t path_lock;

/* Name-buffer cache node */
typedef struct {
	list_entry list;
	void *buf;
} name_cache_t;

static name_cache_t name_cache_head;

/* Called once at boot: set up the page-table cache and related state */
void mm_init_page_table_cache()
{
	unsigned int cr3;
	unsigned int *boot_pgd;
	int i;

	page_table_cache_init(&page_table_cache);
	for (i = 0; i < 1024; i++)
		pgc_entry_count[i] = 0;

	spinlock_init(&mm_lock);
	spinlock_init(&path_lock);
	list_init(&name_cache_head);

	/* Snapshot the boot-time kernel page directory entries.  All task page
	 * directories will be initialised from this copy so they share the same
	 * physical page tables for kernel addresses from the very first switch. */
	LOAD_CR3(cr3);
	boot_pgd = (unsigned int *)(cr3 + KERNEL_OFFSET);
	memcpy(kernel_pgd, &boot_pgd[KERNEL_PAGE_DIR_OFFSET],
	       KERNEL_PGD_ENTRIES * sizeof(unsigned int));
}

/* ---------------------------------------------------------------------------
 * Internal: page directory / page table helpers
 * ---------------------------------------------------------------------------*/

unsigned mm_get_pagedir()
{
	unsigned cr3 = 0;

	LOAD_CR3(cr3);
	return cr3 + KERNEL_OFFSET;
}

typedef struct {
	unsigned int *dir; /* page-directory entry for this address */
	unsigned int *table; /* base of the page table */
	unsigned int *entry; /* page-table entry for this address */
} mm_addr_info;

/*
 * Locate (and optionally allocate) the page-table entry for @addr.
 * Returns 1 on success; 0 if a new page table was needed but allocation failed.
 */
static int mm_get_valid_page_table(unsigned addr, unsigned flag,
				   mm_addr_info *info)
{
	unsigned int *page_dir = (unsigned int *)mm_get_pagedir();
	unsigned offset = ADDR_TO_PGT_OFFSET(addr);

	if ((page_dir[offset] & PAGE_SIZE_MASK) == 0) {
		unsigned int table_addr = mm_alloc_page_table();
		unsigned int pde;

		if (table_addr == 0)
			return 0;
		pde = (table_addr - KERNEL_OFFSET) | PAGE_ENTRY_KERNEL_DATA | flag;
		page_dir[offset] = pde;

		/* Keep the global kernel page directory in sync.  Propagate the
		 * new entry to all existing task page directories so they share
		 * the same physical page table immediately.  This path is hit
		 * only when a brand-new 4 MB kernel region is first accessed —
		 * an exceedingly rare event compared with per-switch memcpy. */
		if (offset >= KERNEL_PAGE_DIR_OFFSET) {
			kernel_pgd[offset - KERNEL_PAGE_DIR_OFFSET] = pde;
			if (kernel_pde_propagator)
				kernel_pde_propagator(offset, pde);
		}
	}
	info->dir = &page_dir[offset];
	info->table =
		(unsigned int *)((*info->dir & PAGE_SIZE_MASK) + KERNEL_OFFSET);
	info->entry = &info->table[ADDR_TO_PET_OFFSET(addr)];
	return 1;
}

/*
 * Write @value into the page-table entry for @addr, allocating a page table if
 * necessary.  Increments the per-table live-entry counter.
 * Returns 1 on success, 0 on allocation failure.
 */
static int mm_set_page_table_entry(unsigned addr, unsigned flag, unsigned value)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(addr, flag, &info))
		return 0;

	/* Track live entries so we know when to reclaim the page table */
	if ((*info.entry & PAGE_SIZE_MASK) == 0) {
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
		int idx = (PAGE_TABLE_CACHE_END - (unsigned)info->table) /
				  PAGE_SIZE -
			  1;
		pgc_entry_count[idx]--;
		if (pgc_entry_count[idx] == 0) {
			mm_free_page_table((unsigned int)info->table);
			*info->dir = 0;
		}
	}
}

/* ---------------------------------------------------------------------------
 * Map management
 * ---------------------------------------------------------------------------*/

/*
 * Add a direct (identity-offset) mapping for a kernel virtual address.
 * Only valid for KERNEL_OFFSET..KERNEL_OFFSET+KERNEL_SIZE.
 * The page-table cache region is permanently mapped and needs no work.
 */
int mm_add_direct_map(unsigned int vir)
{
	unsigned int page_index;

	if (vir < KERNEL_OFFSET)
		return -1;

	/* The page-table cache region is permanently mapped */
	if (vir < PAGE_TABLE_CACHE_END)
		return 1;

	page_index = (vir - KERNEL_OFFSET) / PAGE_SIZE;

	if ((page_index * PAGE_SIZE) > KERNEL_SIZE)
		return -1;

	if (phymm_is_used(page_index))
		return -1;

	if (!mm_set_page_table_entry(
		    vir, 0, (page_index * PAGE_SIZE) | PAGE_ENTRY_KERNEL_DATA))
		return -1;

	phymm_reference_page(page_index);
	return 1;
}

/* Remove a direct kernel mapping and dereference the backing physical page */
void mm_del_direct_map(unsigned int vir)
{
	mm_addr_info info;
	unsigned int phy_addr;

	/* The page-table cache region is permanently mapped */
	if (vir >= KERNEL_OFFSET && vir < PAGE_TABLE_CACHE_END)
		return;

	phy_addr = vir & PAGE_SIZE_MASK;
	if (vir >= KERNEL_OFFSET)
		phy_addr -= KERNEL_OFFSET;

	if (!mm_get_valid_page_table(vir, 0, &info))
		return;

	mm_clear_page_table_entry(&info);
	phymm_dereference_page(phy_addr / PAGE_SIZE);
}

/*
 * Map a firmware physical address (e.g. ACPI tables) into the kernel virtual
 * address space at virtual = phys + KERNEL_OFFSET.
 *
 * Works for any physical address, including those above the initial 8 MB boot
 * mapping.  Does NOT touch the physical allocator reference counts — firmware
 * pages are outside the allocator range and must not be ref-counted.
 *
 * If the page is already covered by the initial boot mapping the function is
 * a no-op and returns 1.  Returns -1 on page-table allocation failure.
 */
int mm_map_phys_page(unsigned int phys)
{
	unsigned int virt = phys + KERNEL_OFFSET;
	mm_addr_info info;

	/* Initial boot mapping covers phys [0, PAGE_TABLE_CACHE_END - KERNEL_OFFSET). */
	if (phys < PAGE_TABLE_CACHE_END - KERNEL_OFFSET)
		return 1;

	if (!mm_get_valid_page_table(virt, 0, &info))
		return -1;

	*info.entry = (phys & PAGE_SIZE_MASK) | PAGE_ENTRY_KERNEL_DATA;
	return 1;
}

/*
 * Map a high physical address (e.g. MMIO resource) into the kernel address
 * space at the same virtual address.
 */
int mm_add_resource_map(unsigned int phy)
{
	mm_addr_info info;

	if (phy < KERNEL_OFFSET)
		return -1;

	if (!mm_get_valid_page_table(phy, 0, &info))
		return -1;

	*info.entry = (phy & PAGE_SIZE_MASK) | PAGE_ENTRY_KERNEL_DATA;
	return 1;
}

/* Remove user-space mappings (page-directory entries 0 and 1) */
void mm_del_user_map()
{
	unsigned int *page_dir = (unsigned int *)mm_get_pagedir();

	page_dir[0] = 0;
	page_dir[1] = 0;
}

/*
 * Map @vir to a physical page.  If @phy is 0 a fresh user page is allocated;
 * otherwise the caller-supplied physical address is used.
 * Returns 1 on success, -1 on failure.
 */
int mm_add_dynamic_map(unsigned int vir, unsigned int phy, unsigned flag)
{
	unsigned int target_phy;
	unsigned int page_index;

	spinlock_lock(&mm_lock);

	if (phy) {
		page_index = (phy & PAGE_SIZE_MASK) / PAGE_SIZE;
		target_phy = phy & PAGE_SIZE_MASK;
	} else {
		page_index = phymm_alloc_user();
		target_phy = page_index * PAGE_SIZE;
	}

	if (!mm_set_page_table_entry(vir, flag | PAGE_ENTRY_WRITABLE,
				     target_phy | flag)) {
		spinlock_unlock(&mm_lock);
		return -1;
	}

	phymm_reference_page(page_index);
	spinlock_unlock(&mm_lock);
	return 1;
}

/* Remove a dynamic user mapping and free the physical page if unreferenced */
void mm_del_dynamic_map(unsigned int vir)
{
	mm_addr_info info;
	unsigned int phy_addr;
	int page_index;

	if (!mm_get_valid_page_table(vir, 0, &info))
		return;

	phy_addr = *info.entry & PAGE_SIZE_MASK;
	page_index = PHY_TO_PAGE_IDX(phy_addr);

	spinlock_lock(&mm_lock);
	if (page_index >= phymm_begin && page_index < phymm_end) {
		if (phymm_is_used(page_index) &&
		    phymm_dereference_page(page_index) == 0)
			phymm_free_user(page_index);
		mm_clear_page_table_entry(&info);
	}
	spinlock_unlock(&mm_lock);
}

/* Return the page-table flags (low 12 bits) for the mapping at @vir */
unsigned mm_get_map_flag(unsigned vir)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(vir, 0, &info))
		return 0;
	return *info.entry & ~PAGE_SIZE_MASK;
}

/* Update the page-table flags for the mapping at @vir */
void mm_set_map_flag(unsigned vir, unsigned flag)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(vir, 0, &info))
		return;
	*info.entry = (*info.entry & PAGE_SIZE_MASK) | flag;
}

/* Return the physical page index backing the virtual address @vir */
unsigned mm_get_attached_page_index(unsigned int vir)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(vir, 0, &info))
		return 0;
	return (*info.entry & PAGE_SIZE_MASK) / PAGE_SIZE;
}

/* ---------------------------------------------------------------------------
 * Virtual memory allocator (kernel heap)
 * ---------------------------------------------------------------------------*/

/* Allocate @page_count contiguous kernel pages; returns virtual base address */
unsigned int vm_alloc(int page_count)
{
	int page_index;
	int i;

	spinlock_lock(&mm_lock);

	page_index = phymm_alloc_kernel(page_count);
	if (page_index < 0) {
		spinlock_unlock(&mm_lock);
		return 0;
	}

	for (i = 0; i < page_count; i++)
		mm_add_direct_map((page_index + i) * PAGE_SIZE + KERNEL_OFFSET);

	RELOAD_CR3();
	spinlock_unlock(&mm_lock);
	return page_index * PAGE_SIZE + KERNEL_OFFSET;
}

/* Release @page_count pages starting at kernel virtual address @vm */
void vm_free(unsigned int vm, int page_count)
{
	int i;

	spinlock_lock(&mm_lock);
	vm &= PAGE_SIZE_MASK;
	for (i = 0; i < page_count; i++)
		mm_del_direct_map(vm + i * PAGE_SIZE);
	RELOAD_CR3();
	phymm_free_kernel((vm - KERNEL_OFFSET) / PAGE_SIZE, page_count);
	spinlock_unlock(&mm_lock);
}

/* Reserve a user virtual address range of @page_count pages for the current task */
unsigned vm_get_usr_zone(unsigned page_count)
{
	// FIXME: only used in load_elf; refactor when elf loading is reworked
	task_struct *cur = CURRENT_TASK();

	return vm_disc_map(cur->user.vm, page_count * PAGE_SIZE);
}

/* ---------------------------------------------------------------------------
 * Name / path buffer cache
 *
 * Caches MAX_PATH-sized buffers to avoid repeated vm_alloc/vm_free calls for
 * temporary pathname storage.
 * ---------------------------------------------------------------------------*/

/* Acquire a pathname buffer (allocates a new one if the cache is empty) */
void *name_get()
{
	void *buf;
	name_cache_t *node;

	spinlock_lock(&path_lock);
	if (list_is_empty(&name_cache_head)) {
		buf = (void *)vm_alloc(MAX_PATH / PAGE_SIZE);
	} else {
		node = list_remove_head(&name_cache_head);
		buf = node->buf;
		free(node);
	}
	spinlock_unlock(&path_lock);
	return buf;
}

/* Return a pathname buffer to the cache */
void name_put(void *buf)
{
	name_cache_t *node;

	spinlock_lock(&path_lock);
	node = calloc(1, sizeof(*node));
	node->buf = buf;
	list_insert_head(&name_cache_head, &node->list);
	spinlock_unlock(&path_lock);
}
