#include <config.h>
#include <lib/rbtree.h>
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
#include <mm/mmu.h>
#include <ps/smp.h>

extern const unsigned __vdso_start;
extern const unsigned __vdso_end;

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

typedef struct _mm_cache_t {
	unsigned int top;
	unsigned int count;
	unsigned int total;
	unsigned int mem[0];
} mm_cache_t;

typedef struct _page_table_cache_t {
	mm_cache_t hdr;
	unsigned int mem[PAGE_TABLE_CACHE_PAGES];
} page_table_cache_t;

static page_table_cache_t page_table_cache;
static unsigned int kernel_pde_tables[1024 - KERNEL_PAGE_DIR_OFFSET];

#define KERNEL_KMAP_PAGES ((KERNEL_KMAP_END - KERNEL_KMAP_BEGIN) / PAGE_SIZE)

typedef struct _kmap_cache_t {
	mm_cache_t hdr;
	unsigned int mem[KERNEL_KMAP_PAGES];
} kmap_cache_t;

typedef struct _kmap_loopup_entry {
	struct rb_node node;
	paddr_t phys;
	vaddr_t virt;
	unsigned int ref;
} kmap_loopup_entry;

static kmap_cache_t kmap_phy_cache;
static struct rb_root kmap_phy_to_virt = _RBTREE_ROOT_INIT;

static kmap_loopup_entry *kmap_cache_find(paddr_t phy_address)
{
	struct rb_node *n = kmap_phy_to_virt.rb_node;
	while (n) {
		kmap_loopup_entry *e = rb_entry(n, kmap_loopup_entry, node);
		if (phy_address < e->phys)
			n = n->rb_left;
		else if (phy_address > e->phys)
			n = n->rb_right;
		else
			return e;
	}
	return NULL;
}

static void kmap_cache_insert(kmap_loopup_entry *entry)
{
	struct rb_node **link = &kmap_phy_to_virt.rb_node, *parent = NULL;
	while (*link) {
		kmap_loopup_entry *e = rb_entry(*link, kmap_loopup_entry, node);
		parent = *link;
		link = entry->phys < e->phys ? &(*link)->rb_left :
					       &(*link)->rb_right;
	}
	rb_link_node(&entry->node, parent, link);
	rb_insert_color(&entry->node, &kmap_phy_to_virt);
}

static void mm_cache_init(mm_cache_t *cache, unsigned int begin,
			  unsigned int total)
{
	int i;

	for (i = 0; i < total; i++)
		cache->mem[i] = begin + i * PAGE_SIZE;
	cache->top = 0;
	cache->count = total;
	cache->total = total;
}

static unsigned int mm_cache_alloc(mm_cache_t *cache)
{
	unsigned int ret;

	if (cache->count == 0)
		return 0;
	ret = cache->mem[cache->top];
	__sync_add_and_fetch(&cache->top, 1);
	pgc_top = __sync_add_and_fetch(&cache->count, -1);
	cache_count++;
	return ret;
}

static void mm_cache_free(mm_cache_t *cache, unsigned int val)
{
	/* A cache is a bounded stack.  Reject duplicate/foreign frees instead of
	 * allowing top to underflow and poisoning all subsequent allocations. */
	if (cache->count >= cache->total || cache->top == 0)
		return;
	__sync_add_and_fetch(&cache->top, -1);
	cache->mem[cache->top] = val;
	pgc_top = __sync_add_and_fetch(&cache->count, 1);
	cache_count--;
}

vaddr_t mm_alloc_page_table(void)
{
	unsigned int ret = mm_cache_alloc((mm_cache_t *)&page_table_cache);

	if (ret == 0) {
		klog("mm_alloc_page_table: page table cache exhausted\n");
		return 0;
	}
	memset((void *)ret, 0, PAGE_SIZE);
	return ret;
}

void mm_free_page_table(vaddr_t vir)
{
	mm_cache_free((mm_cache_t *)&page_table_cache, vir);
}

static void kmap_cache_erase(paddr_t phy_address)
{
	kmap_loopup_entry *entry = kmap_cache_find(phy_address);
	if (entry) {
		rb_erase(&entry->node, &kmap_phy_to_virt);
		kfree(entry);
	}
}

static void mm_init_kernel_page_dir_template(void)
{
	pte_t *page_dir = (pte_t *)mm_get_pagedir();
	unsigned int i;

	for (i = 0; i < 1024 - KERNEL_PAGE_DIR_OFFSET; i++) {
		if (page_dir[KERNEL_PAGE_DIR_OFFSET + i] & PAGE_ENTRY_PRESENT)
			continue;

		unsigned int table_addr =
			mm_cache_alloc((mm_cache_t *)&page_table_cache);

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
static int mm_dynamic_region(paddr_t phy);

/* Name-buffer cache node */

static list_entry name_cache_head;

/* Called once at boot: set up the page-table cache and related state */
void mm_init_cache()
{
	int i;

	mm_cache_init((mm_cache_t *)&page_table_cache, PAGE_TABLE_CACHE_BEGIN,
		      PAGE_TABLE_CACHE_PAGES);
	memset(kernel_pde_tables, 0, sizeof(kernel_pde_tables));
	mm_init_kernel_page_dir_template();
	for (i = 0; i < PAGE_TABLE_CACHE_PAGES; i++)
		pgc_entry_count[i] = 0;

	mm_cache_init((mm_cache_t *)&kmap_phy_cache, KERNEL_KMAP_BEGIN,
		      KERNEL_KMAP_PAGES);

	spinlock_init(&mm_lock);
	spinlock_init(&path_lock);
	spinlock_init(&kmap_lock);
	list_init(&name_cache_head);
}

void mm_init_process_page_dir(vaddr_t page_dir)
{
	pte_t *dst = (pte_t *)page_dir;
	pte_t *src = (pte_t *)mm_get_pagedir();

	memset(dst, 0, PAGE_SIZE);
	memcpy(&dst[KERNEL_PAGE_DIR_OFFSET], &src[KERNEL_PAGE_DIR_OFFSET],
	       (1024 - KERNEL_PAGE_DIR_OFFSET) * sizeof(pte_t));
}

/*
 * Internal: page directory / page table helpers
 */

vaddr_t mm_get_pagedir(void)
{
	return arch_mm_current_address_space() + KERNEL_OFFSET;
}

vaddr_t mm_phys_to_virt(paddr_t phys)
{
	paddr_t page = phys & PAGE_SIZE_MASK;
	unsigned int off = ADDR_TO_PAGE_OFFSET(phys);
	kmap_loopup_entry *entry = NULL;
	int irq;

	if (page < KERNEL_DIRECT_MAP_LIMIT)
		return KERNEL_OFFSET + page + off;

	spinlock_lock(&kmap_lock, &irq);
	entry = kmap_cache_find(page);
	if (entry) {
		vaddr_t ret = entry->virt + off;
		spinlock_unlock(&kmap_lock, irq);
		return ret;
	}
	spinlock_unlock(&kmap_lock, irq);

	if (mm_kmap_phys(page) == 1) {
		entry = kmap_cache_find(page);
		if (entry) {
			return entry->virt + off;
		}
	}

	return 0;
}

typedef struct {
	pte_t *dir; /* page-directory entry for this address */
	pte_t *table; /* base of the page table */
	pte_t *entry; /* page-table entry for this address */
} mm_addr_info;

static int mm_get_valid_page_table_in_dir(pte_t *page_dir, vaddr_t addr,
					  mm_addr_info *info)
{
	unsigned offset = ADDR_TO_PGT_OFFSET(addr);

	info->dir = info->table = info->entry = NULL;
	if (!page_dir)
		return 0;

	info->dir = &page_dir[offset];
	if ((*info->dir & PAGE_SIZE_MASK) == 0)
		return 0;

	info->table = (pte_t *)PHY_TO_VIRT(*info->dir & PAGE_SIZE_MASK);
	info->entry = &info->table[ADDR_TO_PET_OFFSET(addr)];
	return 1;
}

/*
 * Locate (and optionally allocate) the page-table entry for @addr.
 * Returns 1 on success; 0 if a new page table was needed but allocation failed.
 */
static int mm_get_valid_page_table(vaddr_t addr, unsigned flag,
				   mm_addr_info *info, int alloc_if_none)
{
	pte_t *page_dir = (pte_t *)mm_get_pagedir();
	unsigned offset = ADDR_TO_PGT_OFFSET(addr);

	info->dir = info->table = info->entry = NULL;

	if ((page_dir[offset] & PAGE_SIZE_MASK) == 0) {
		if (!alloc_if_none)
			return 0;

		vaddr_t table_addr = mm_alloc_page_table();
		pte_t pde;

		if (table_addr == 0)
			return 0;
		pde = VIRT_TO_PHY(table_addr) | PAGE_ENTRY_KERNEL_DATA | flag;
		page_dir[offset] = pde;
	}
	info->dir = &page_dir[offset];
	if (*info->dir)
		info->table = (pte_t *)PHY_TO_VIRT(*info->dir &
							  PAGE_SIZE_MASK);
	if (info->table)
		info->entry = &info->table[ADDR_TO_PET_OFFSET(addr)];

	return info->entry != NULL;
}

paddr_t mm_virt_to_phys(vaddr_t virt)
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
static int mm_set_page_table_entry(vaddr_t addr, unsigned flag, pte_t value)
{
	mm_addr_info info;
	pte_t old;

	if (!mm_get_valid_page_table(addr, flag, &info, 1))
		return 0;

	/* Track live entries so we know when to reclaim the page table */
	if (addr < KERNEL_OFFSET && (*info.entry & PAGE_SIZE_MASK) == 0) {
		int idx = (PAGE_TABLE_CACHE_END - (uintptr_t)info.table) /
				  PAGE_SIZE -
			  1;
		pgc_entry_count[idx]++;
	}

	old = *info.entry;
	*info.entry = value;
	/* A newly-present entry cannot have a stale TLB translation. */
	if (old & PAGE_ENTRY_PRESENT)
		arch_mm_invalidate(addr);
	return 1;
}

/*
 * Zero the page-table entry and decrement the live-entry counter.
 * Reclaims the page table itself when the last entry is removed.
 */
static void mm_clear_page_table_entry(mm_addr_info *info)
{
	paddr_t phy = *info->entry & PAGE_SIZE_MASK;
	unsigned dir_index =
		(unsigned)(info->dir - (pte_t *)mm_get_pagedir());
	vaddr_t addr = ((vaddr_t)dir_index << MOS_PGT_SHIFT) |
		((vaddr_t)(info->entry - info->table) << MOS_PET_SHIFT);

	*info->entry = 0;
	arch_mm_invalidate(addr);
	if (phy) {
		if (dir_index < KERNEL_PAGE_DIR_OFFSET) {
			int idx =
				(PAGE_TABLE_CACHE_END - (uintptr_t)info->table) /
					PAGE_SIZE -
				1;
			pgc_entry_count[idx]--;
			if (pgc_entry_count[idx] == 0) {
				*info->dir = 0;
				mm_free_page_table((unsigned int)info->table);
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
int mm_kmap_page(vaddr_t vir)
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
void mm_kunmap_page(vaddr_t vir)
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
int mm_kmap_phys(paddr_t phys)
{
	paddr_t page = phys & PAGE_SIZE_MASK;
	vaddr_t virt;
	kmap_loopup_entry *entry = NULL;
	int irq;
	mm_addr_info info;

	if (page < KERNEL_DIRECT_MAP_LIMIT) {
		virt = KERNEL_OFFSET + page;
		if (!mm_get_valid_page_table(virt, 0, &info, 1)) {
			klog("mm_kmap_phys: page table alloc failed phys=%x virt=%x\n",
			     phys, virt);
			return -1;
		}
		*info.entry = page | PAGE_ENTRY_KERNEL_DATA;
		arch_mm_invalidate(virt);
		return 1;
	}

	spinlock_lock(&kmap_lock, &irq);
	entry = kmap_cache_find(page);
	if (entry) {
		entry->ref++;
		spinlock_unlock(&kmap_lock, irq);
		return 1;
	}

	virt = mm_cache_alloc((mm_cache_t *)&kmap_phy_cache);
	if (virt == 0) {
		spinlock_unlock(&kmap_lock, irq);
		klog("mm_kmap_phys: kmap window exhausted\n");
		return -1;
	}

	if (!mm_get_valid_page_table(virt, 0, &info, 1)) {
		mm_cache_free((mm_cache_t *)&kmap_phy_cache, virt);
		spinlock_unlock(&kmap_lock, irq);
		klog("mm_kmap_phys: page table alloc failed phys=%x virt=%x\n",
		     page, virt);
		return -1;
	}

	*info.entry = page | PAGE_ENTRY_KERNEL_DATA;
	arch_mm_invalidate(virt);

	// add an entry
	entry = kmalloc(sizeof(*entry));
	if (!entry) {
		*info.entry = 0;
		arch_mm_invalidate(virt);
		mm_cache_free((mm_cache_t *)&kmap_phy_cache, virt);
		spinlock_unlock(&kmap_lock, irq);
		return -1;
	}
	rb_init_node(&entry->node);
	entry->phys = page;
	entry->virt = virt;
	entry->ref = 1;
	kmap_cache_insert(entry);
	spinlock_unlock(&kmap_lock, irq);
	return 1;
}

void mm_kunmap_phys(paddr_t phys)
{
	paddr_t page = phys & PAGE_SIZE_MASK;
	kmap_loopup_entry *entry = NULL;
	vaddr_t virt;
	int irq;
	mm_addr_info info;

	if (page < KERNEL_DIRECT_MAP_LIMIT)
		return;

	spinlock_lock(&kmap_lock, &irq);
	entry = kmap_cache_find(page);
	if (!entry) {
		goto done;
	}

	entry->ref--;
	if (entry->ref > 0) {
		goto done;
	}

	virt = entry->virt;
	if (mm_get_valid_page_table(virt, 0, &info, 0)) {
		*info.entry = 0;
		arch_mm_invalidate(virt);
	}

	/* Return the virtual slot to the kmap allocator.  The cache contains
	 * virtual addresses (not physical page numbers); passing @page here
	 * silently corrupts the free-slot stack and typically only becomes
	 * visible once allocations spill above the low direct-map window. */
	mm_cache_free((mm_cache_t *)&kmap_phy_cache, virt);
	kmap_cache_erase(page);

done:
	spinlock_unlock(&kmap_lock, irq);
}

/*
 * Map a high physical address (e.g. MMIO resource) into the kernel address
 * space at the same virtual address.
 */
int mm_map_io(paddr_t phy)
{
	mm_addr_info info;

	/* Fixed-address MMIO lives above the kmap allocator.  Keeping these
	 * windows disjoint prevents highmem aliases from replacing PCI BAR PTEs. */
	if (phy < KERNEL_IO_BEGIN)
		return -1;

	if (!mm_get_valid_page_table(phy, 0, &info, 1)) {
		klog("mm_map_io: page table alloc failed phy=%x\n", phy);
		return -1;
	}

	*info.entry = (phy & PAGE_SIZE_MASK) | PAGE_ENTRY_KERNEL_DATA;
	return 1;
}

/* Remove the temporary low identity map installed during boot. */
void mm_del_user_map()
{
	pte_t *page_dir = (pte_t *)mm_get_pagedir();
	unsigned int reserved_page_tables =
		(RESERVED_PAGES + PE_TABLE_SIZE - 1) / PE_TABLE_SIZE;
	unsigned int i;

	for (i = 0; i < reserved_page_tables; i++)
		page_dir[i] = 0;
	arch_mm_flush_local();
}

/*
 * Destroy every user-space mapping in @page_dir in one pass.
 *
 * This is faster than repeated mm_unmap_page() calls during exit/exec because
 * it walks each user page table exactly once, drops all backing user pages,
 * then frees the page table as a whole.
 */
void mm_destroy_user_map(vaddr_t page_dir)
{
	pte_t *dir = (pte_t *)page_dir;
	int irq;
	unsigned int i;
	unsigned int dynamic_begin = phymm_begin * PAGE_SIZE;
	unsigned int dynamic_end = phymm_end * PAGE_SIZE;
	unsigned int vdso_begin = VIRT_TO_PHY(&__vdso_start);
	unsigned int vdso_end = VIRT_TO_PHY(&__vdso_end);

	if (!dir)
		return;
	spinlock_lock(&mm_lock, &irq);
	for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
		pte_t *table;
		paddr_t table_phy;
		unsigned int j;
		int cache_idx;

		table_phy = dir[i] & PAGE_SIZE_MASK;
		if (!table_phy)
			continue;

		table = (pte_t *)PHY_TO_VIRT(table_phy);
		cache_idx =
			(PAGE_TABLE_CACHE_END - (uintptr_t)table) / PAGE_SIZE -
			1;
		/* The address space is inactive before its last reference is dropped. */
		dir[i] = 0;
		/* The live-entry counter is maintained for every user mapping.  Most
		 * page tables created during short-lived exec/clone paths are already
		 * empty by the time the address space is destroyed; avoid needlessly
		 * scanning all 1024 PTEs in that case. */
		if (pgc_entry_count[cache_idx] != 0)
			for (j = 0; j < PG_TABLE_SIZE; j++) {
				paddr_t phy_addr = table[j] &
							PAGE_SIZE_MASK;
				unsigned int page_index;

				if (!phy_addr)
					continue;

				page_index = PHY_TO_PAGE_IDX(phy_addr);
				if ((phy_addr >= dynamic_begin &&
				     phy_addr < dynamic_end) ||
				    (phy_addr >= vdso_begin &&
				     phy_addr < vdso_end)) {
					/* Every page installed through mm_map_page carries a reference;
				 * decrement once directly instead of doing a separate atomic
				 * read via phymm_is_used(). */
					if (phymm_dereference_page(
						    page_index) == 0)
						phymm_free_user(page_index);
				}
			}

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
int mm_map_page(vaddr_t vir, paddr_t phy, unsigned flag)
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
			if (phymm_reclaim_user_cache(32) == 0) {
				klog("mm_map_page: phymm_alloc_user failed vir=%x flag=%x\n",
				     vir, flag);
				return -1;
			}
			spinlock_lock(&mm_lock, &irq);
			page_index = phymm_alloc_user();
			if (page_index == PHYMM_INVALID) {
				spinlock_unlock(&mm_lock, irq);
				klog("mm_map_page: phymm_alloc_user failed after reclaim vir=%x flag=%x\n",
				     vir, flag);
				return -1;
			}
		}
		target_phy = page_index * PAGE_SIZE;
	}

	if (!mm_set_page_table_entry(vir, flag | PAGE_ENTRY_WRITABLE,
				     target_phy | flag)) {
		if (!phy)
			phymm_free_user(page_index);
		spinlock_unlock(&mm_lock, irq);
		klog("mm_map_page: page table alloc failed vir=%x phy=%x flag=%x\n",
		     vir, target_phy, flag);
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
int mm_map_page_io(vaddr_t vir, paddr_t phy, unsigned flag)
{
	int irq;

	spinlock_lock(&mm_lock, &irq);
	if (!mm_set_page_table_entry(vir, flag,
				     (phy & PAGE_SIZE_MASK) | flag)) {
		spinlock_unlock(&mm_lock, irq);
		klog("mm_map_page_io: page table alloc failed vir=%x phy=%x flag=%x\n",
		     vir, phy, flag);
		return -1;
	}
	spinlock_unlock(&mm_lock, irq);
	return 1;
}

static int mm_dynamic_region(paddr_t phy)
{
	paddr_t begin = (paddr_t)phymm_begin * PAGE_SIZE;
	paddr_t end = (paddr_t)phymm_end * PAGE_SIZE;
	return phy >= begin && phy < end;
}

/* Remove a dynamic user mapping and free the physical page if unreferenced */
void mm_unmap_page(vaddr_t vir)
{
	mm_addr_info info;
	paddr_t phy_addr;
	int page_index;
	int irq;

	if (!mm_get_valid_page_table(vir, 0, &info, 0))
		return;

	phy_addr = *info.entry & PAGE_SIZE_MASK;
	page_index = PHY_TO_PAGE_IDX(phy_addr);

	spinlock_lock(&mm_lock, &irq);
	mm_clear_page_table_entry(&info);
	if (mm_dynamic_region(phy_addr) || mm_vdso_region(phy_addr)) {
		if (phymm_is_used(page_index) &&
		    phymm_dereference_page(page_index) == 0)
			phymm_free_user(page_index);
	}
	spinlock_unlock(&mm_lock, irq);
}

/* Return the page-table flags (low 12 bits) for the mapping at @vir */
unsigned mm_get_map_flag(vaddr_t vir)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(vir, 0, &info, 0))
		return 0;
	return *info.entry & ~PAGE_SIZE_MASK;
}

unsigned mm_get_map_flag_pd(vaddr_t page_dir, vaddr_t vir)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table_in_dir((pte_t *)page_dir, vir,
					    &info))
		return 0;
	return *info.entry & ~PAGE_SIZE_MASK;
}

/* Update the page-table flags for the mapping at @vir */
void mm_set_map_flag(vaddr_t vir, unsigned flag)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table(vir, 0, &info, 0))
		return;
	*info.entry = (*info.entry & PAGE_SIZE_MASK) | flag;
	arch_mm_invalidate(vir);
}

void mm_set_map_flag_pd(vaddr_t page_dir, vaddr_t vir, unsigned flag)
{
	mm_addr_info info;

	if (!mm_get_valid_page_table_in_dir((pte_t *)page_dir, vir,
					    &info))
		return;
	*info.entry = (*info.entry & PAGE_SIZE_MASK) | flag;
	if ((pte_t *)page_dir == (pte_t *)mm_get_pagedir())
		arch_mm_invalidate(vir);
}

/* Return the physical page index backing the virtual address @vir */
pfn_t mm_get_attached_page_index(vaddr_t vir)
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
vaddr_t vm_alloc(int page_count)
{
	int page_index;
	int i;
	int irq;
	unsigned int vm;

	spinlock_lock(&mm_lock, &irq);

	page_index = phymm_alloc_kernel(page_count);
	if (page_index == PHYMM_INVALID) {
		spinlock_unlock(&mm_lock, irq);
		if (phymm_reclaim_kernel_cache(32) == 0) {
			klog("vm_alloc: phymm_alloc_kernel failed page_count=%d\n",
			     page_count);
			return 0;
		}
		spinlock_lock(&mm_lock, &irq);
		page_index = phymm_alloc_kernel(page_count);
		if (page_index == PHYMM_INVALID) {
			spinlock_unlock(&mm_lock, irq);
			klog("vm_alloc: phymm_alloc_kernel failed after reclaim page_count=%d\n",
			     page_count);
			return 0;
		}
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
			klog("vm_alloc: mm_kmap_page failed page_count=%d page_index=%u i=%d vm=%x\n",
			     page_count, page_index, i, vm);
			return 0;
		}
		phymm_reference_page(page_index + i);
	}

	spinlock_unlock(&mm_lock, irq);

	buffer_count += page_count;
	return vm;
}

/* Release @page_count pages starting at kernel virtual address @vm */
void vm_free(vaddr_t vm, int page_count)
{
	int i;
	int irq;
	unsigned int page_index;

	spinlock_lock(&mm_lock, &irq);
	vm &= PAGE_SIZE_MASK;
	page_index = VIRT_TO_PAGE_IDX(vm);
	for (i = 0; i < page_count; i++) {
		paddr_t phy = VIRT_TO_PHY(vm + i * PAGE_SIZE);

		mm_kunmap_page(vm + i * PAGE_SIZE);
		if (phy)
			phymm_dereference_page(PHY_TO_PAGE_IDX(phy));
	}
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
