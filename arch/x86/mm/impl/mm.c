#include <config.h>
#include <lib/rbtree.h>
#include <boot/multiboot.h>
#include <lib/klib.h>
#include <lib/list.h>
#include <lib/lock.h>

static spinlock_t table_lock;
#include <ps/ps.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/vdso.h>
#include <macro.h>
#include <mm/mmu.h>
#include <ps/smp.h>
#include <int/int.h>

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
static pte_t *kernel_page_dir;
static int direct_map_large;
/* Page directories are allocated from the low kernel pool. Track their PFNs
 * without allocating memory, including directories not yet attached to tasks.
 * mm_lock protects directory lifetime and shared kernel mappings. */
static unsigned int process_dirs[KERNEL_DIRECT_MAP_LIMIT / PAGE_SIZE / 32];

static int mm_large_entry(pte_t entry)
{
	return (entry & (PAGE_ENTRY_PRESENT | PAGE_ENTRY_LARGE)) ==
	       (PAGE_ENTRY_PRESENT | PAGE_ENTRY_LARGE);
}

static void mm_init_direct_map(void)
{
	unsigned i;

	kernel_page_dir = (pte_t *)mm_get_pagedir();
	direct_map_large = arch_mm_enable_large_pages();
	if (!direct_map_large)
		return;
	/* Install before process directories or APs exist. Bootstrap tables live
	 * outside the page-table cache and must not be returned to that cache. */
	for (i = 0; i < KERNEL_DIRECT_MAP_LIMIT / LARGE_PAGE_SIZE; i++)
		kernel_page_dir[KERNEL_PAGE_DIR_OFFSET + i] =
			i * LARGE_PAGE_SIZE | PAGE_ENTRY_KERNEL_DATA |
			PAGE_ENTRY_LARGE;
	arch_mm_flush_local();
}

/* A permission change needs a 4 KiB leaf. Publish the same table into every
 * directory, so subsequent PTE changes remain shared across address spaces. */
static int mm_split_direct_page(pte_t *dir, vaddr_t addr)
{
	unsigned index = ADDR_TO_PGT_OFFSET(addr), i, word;
	pte_t old = dir[index];
	pte_t *table;
	vaddr_t table_addr;
	pte_t entry;

	if (!mm_large_entry(old))
		return 1;
	if (addr < KERNEL_OFFSET || addr >= KERNEL_KMAP_BEGIN)
		return 0;
	table_addr = mm_alloc_page_table();
	if (!table_addr)
		return 0;
	table = (pte_t *)table_addr;
	for (i = 0; i < PE_TABLE_SIZE; i++)
		table[i] = ((old & LARGE_PAGE_MASK) + i * PAGE_SIZE) |
			   (old & (PAGE_SIZE - 1) & ~PAGE_ENTRY_LARGE);
	entry = VIRT_TO_PHY(table_addr) | PAGE_ENTRY_PAGE_TABLE;
	kernel_pde_tables[index - KERNEL_PAGE_DIR_OFFSET] = table_addr;
	kernel_page_dir[index] = entry;
	for (word = 0; word < sizeof(process_dirs) / sizeof(process_dirs[0]);
	     word++) {
		unsigned bits = process_dirs[word];
		while (bits) {
			unsigned bit = __builtin_ctz(bits);
			pte_t *pd = (pte_t *)(KERNEL_OFFSET +
					      (word * 32 + bit) * PAGE_SIZE);
			pd[index] = entry;
			bits &= bits - 1;
		}
	}
	dir[index] = entry;
	smp_tlb_flush();
	return 1;
}

#define KERNEL_KMAP_PAGES ((KERNEL_KMAP_END - KERNEL_KMAP_BEGIN) / PAGE_SIZE)
#define COPY_SLOT_PAGES (2 * SMP_MAX_CPUS)
#define COPY_SLOT_BEGIN (KERNEL_KMAP_END - COPY_SLOT_PAGES * PAGE_SIZE)
static pte_t *copy_slot_ptes[COPY_SLOT_PAGES];

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
	int irq;
	spinlock_lock(&table_lock, &irq);
	unsigned int ret = mm_cache_alloc((mm_cache_t *)&page_table_cache);
	spinlock_unlock(&table_lock, irq);

	if (ret == 0) {
		klog("mm_alloc_page_table: page table cache exhausted\n");
		return 0;
	}
	memset((void *)ret, 0, PAGE_SIZE);
	return ret;
}

void mm_free_page_table(vaddr_t vir)
{
	int irq;
	spinlock_lock(&table_lock, &irq);
	mm_cache_free((mm_cache_t *)&page_table_cache, vir);
	spinlock_unlock(&table_lock, irq);
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
						       PAGE_ENTRY_PAGE_TABLE;
	}
}

static void mm_mark_kernel_pages_global(void)
{
	pte_t *page_dir = (pte_t *)mm_get_pagedir();
	unsigned int i;

	for (i = KERNEL_PAGE_DIR_OFFSET; i < PG_TABLE_SIZE; i++) {
		pte_t *page_table;
		unsigned int j;

		if (!(page_dir[i] & PAGE_ENTRY_PRESENT))
			continue;
		if (mm_large_entry(page_dir[i])) {
			page_dir[i] |= PAGE_ENTRY_GLOBAL;
			continue;
		}
		page_table = (pte_t *)PHY_TO_VIRT(page_dir[i] & PAGE_SIZE_MASK);
		for (j = 0; j < PE_TABLE_SIZE; j++)
			if (page_table[j] & PAGE_ENTRY_PRESENT)
				page_table[j] |= PAGE_ENTRY_GLOBAL;
	}
}

/*
 * Locks and initialisation
 */

spinlock_t mm_lock;
static spinlock_t path_lock;
static spinlock_t kmap_lock;
static int mm_dynamic_region(paddr_t phy);

/* Name-buffer cache node */

static list_entry name_cache_head;

/* Called once at boot: set up the page-table cache and related state */
void mm_init_cache()
{
	spinlock_init(&table_lock);
	int i;

	mm_cache_init((mm_cache_t *)&page_table_cache, PAGE_TABLE_CACHE_BEGIN,
		      PAGE_TABLE_CACHE_PAGES);
	mm_init_direct_map();
	/* The bootstrap mappings predate PAGE_ENTRY_KERNEL_DATA.  Mark their
	 * high-half aliases global before process page directories copy them. */
	mm_mark_kernel_pages_global();
	memset(kernel_pde_tables, 0, sizeof(kernel_pde_tables));
	mm_init_kernel_page_dir_template();
	for (i = 0; i < COPY_SLOT_PAGES; i++) {
		vaddr_t addr = COPY_SLOT_BEGIN + i * PAGE_SIZE;
		pte_t pde = kernel_page_dir[ADDR_TO_PGT_OFFSET(addr)];
		if (pde & PAGE_ENTRY_PRESENT)
			copy_slot_ptes[i] = (pte_t *)(KERNEL_OFFSET +
						      (pde & PAGE_SIZE_MASK)) +
					    ADDR_TO_PET_OFFSET(addr);
	}
	for (i = 0; i < PAGE_TABLE_CACHE_PAGES; i++)
		pgc_entry_count[i] = 0;

	mm_cache_init((mm_cache_t *)&kmap_phy_cache, KERNEL_KMAP_BEGIN,
		      KERNEL_KMAP_PAGES - COPY_SLOT_PAGES);

	spinlock_init(&mm_lock);
	spinlock_init(&path_lock);
	spinlock_init(&kmap_lock);
	list_init(&name_cache_head);
}

void mm_init_process_page_dir(vaddr_t page_dir)
{
	pte_t *dst = (pte_t *)page_dir;
	pte_t *src = kernel_page_dir;
	unsigned pfn = (page_dir - KERNEL_OFFSET) / PAGE_SIZE;

	int irq;
	spinlock_lock(&mm_lock, &irq);
	memset(dst, 0, PAGE_SIZE);
	memcpy(&dst[KERNEL_PAGE_DIR_OFFSET], &src[KERNEL_PAGE_DIR_OFFSET],
	       (1024 - KERNEL_PAGE_DIR_OFFSET) * sizeof(pte_t));
	process_dirs[pfn / 32] |= 1U << (pfn % 32);
	spinlock_unlock(&mm_lock, irq);
}

int mm_copy_phys_page(paddr_t dst, paddr_t src)
{
	unsigned irq, slot;
	vaddr_t addr;
	pte_t *src_pte, *dst_pte;

	if ((dst | src) & (PAGE_SIZE - 1))
		return 0;
	/* The entire mapping lifetime is this non-sleeping copy. With IRQs off
	 * there is no migration or interrupt nesting that could reuse the slots.
	 * Slots are disjoint per CPU, shared in all page directories, and never
	 * handed to callers or entered in the general kmap lookup tree. */
	irq = int_intr_disable();
	slot = 2 * smp_cpu_id();
	addr = COPY_SLOT_BEGIN + slot * PAGE_SIZE;
	src_pte = copy_slot_ptes[slot];
	dst_pte = copy_slot_ptes[slot + 1];
	if (!src_pte || !dst_pte) {
		int_intr_setlevel(irq);
		return 0;
	}
	*src_pte = src | PAGE_ENTRY_PRESENT;
	*dst_pte = dst | PAGE_ENTRY_PRESENT | PAGE_ENTRY_WRITABLE;
	arch_mm_invalidate(addr);
	arch_mm_invalidate(addr + PAGE_SIZE);
	memcpy((void *)(addr + PAGE_SIZE), (void *)addr, PAGE_SIZE);
	*src_pte = *dst_pte = 0;
	arch_mm_invalidate(addr);
	arch_mm_invalidate(addr + PAGE_SIZE);
	int_intr_setlevel(irq);
	return 1;
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
	if (!(*info->dir & PAGE_ENTRY_PRESENT) || mm_large_entry(*info->dir))
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

	if (mm_large_entry(page_dir[offset]))
		return 0;
	if (!(page_dir[offset] & PAGE_ENTRY_PRESENT)) {
		if (!alloc_if_none)
			return 0;

		vaddr_t table_addr = mm_alloc_page_table();
		pte_t pde;

		if (table_addr == 0)
			return 0;
		pde = VIRT_TO_PHY(table_addr) | PAGE_ENTRY_PAGE_TABLE | flag;
		page_dir[offset] = pde;
	}
	info->dir = &page_dir[offset];
	if (*info->dir)
		info->table = (pte_t *)PHY_TO_VIRT(*info->dir & PAGE_SIZE_MASK);
	if (info->table)
		info->entry = &info->table[ADDR_TO_PET_OFFSET(addr)];

	return info->entry != NULL;
}

paddr_t mm_virt_to_phys(vaddr_t virt)
{
	mm_addr_info info;
	pte_t pde = ((pte_t *)mm_get_pagedir())[ADDR_TO_PGT_OFFSET(virt)];

	if (mm_large_entry(pde))
		return (pde & LARGE_PAGE_MASK) | (virt & (LARGE_PAGE_SIZE - 1));

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
	unsigned dir_index = (unsigned)(info->dir - (pte_t *)mm_get_pagedir());
	vaddr_t addr = ((vaddr_t)dir_index << MOS_PGT_SHIFT) |
		       ((vaddr_t)(info->entry - info->table) << MOS_PET_SHIFT);

	*info->entry = 0;
	arch_mm_invalidate(addr);
	if (phy) {
		if (dir_index < KERNEL_PAGE_DIR_OFFSET) {
			int idx = (PAGE_TABLE_CACHE_END -
				   (uintptr_t)info->table) /
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

	/* Large-page direct mappings remain present across allocator reuse. */
	if (direct_map_large)
		return 1;

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

	if (direct_map_large && vir >= KERNEL_OFFSET && vir < KERNEL_KMAP_BEGIN)
		return;

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
		if (direct_map_large)
			return 1;
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
	/* Enabling PGE after the identity-map flush prevents its shared bootstrap
	 * PTEs from ever surviving as global low-address translations. */
	arch_mm_enable_global_pages();
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
		unsigned int remaining;
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
		/* Stop after the final live entry; trailing empty PTEs own no pages. */
		remaining = pgc_entry_count[cache_idx];
		for (j = 0; j < PG_TABLE_SIZE && remaining; j++) {
			paddr_t phy_addr = table[j] & PAGE_SIZE_MASK;
			unsigned int page_index;

			if (!phy_addr)
				continue;
			remaining--;
			if (table[j] & PAGE_ENTRY_DIRECT_PHYS)
				continue;

			page_index = PHY_TO_PAGE_IDX(phy_addr);
			if ((phy_addr >= dynamic_begin &&
			     phy_addr < dynamic_end) ||
			    (phy_addr >= vdso_begin && phy_addr < vdso_end)) {
				/* Every page installed through mm_map_page carries a reference;
			 * decrement once directly instead of doing a separate atomic
			 * read via phymm_is_used(). */
				if (phymm_dereference_page(page_index) == 0)
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

/* Skip absent page-directory entries while locating resident pages. */
vaddr_t mm_next_mapped_page(vaddr_t begin, vaddr_t end)
{
	vaddr_t address = begin;
	int irq;
	spinlock_lock(&mm_lock, &irq);
	while (address < end) {
		pte_t entry = ((pte_t *)mm_get_pagedir())[ADDR_TO_PGT_OFFSET(address)];
		vaddr_t next;
		if (!(entry & PAGE_ENTRY_PRESENT))
			next = (address | (((vaddr_t)1 << 22) - 1)) + 1;
		else if (mm_get_map_flag(address) & PAGE_ENTRY_PRESENT)
			break;
		else
			next = address + PAGE_SIZE;
		address = next > address && next < end ? next : end;
	}
	spinlock_unlock(&mm_lock, irq);
	return address;
}

/* Remove a dynamic user mapping and release unreferenced physical pages. */
void mm_unmap_page(vaddr_t vir)
{
	mm_addr_info info;
	paddr_t phy_addr;
	int page_index;
	int irq;
	unsigned flags;

	spinlock_lock(&mm_lock, &irq);
	if (!mm_get_valid_page_table(vir, 0, &info, 0)) {
		spinlock_unlock(&mm_lock, irq);
		return;
	}

	phy_addr = *info.entry & PAGE_SIZE_MASK;
	flags = *info.entry;
	page_index = PHY_TO_PAGE_IDX(phy_addr);

	mm_clear_page_table_entry(&info);
	if (!(flags & PAGE_ENTRY_DIRECT_PHYS) &&
	    (mm_dynamic_region(phy_addr) || mm_vdso_region(phy_addr))) {
		if (phymm_is_used(page_index) &&
		    phymm_dereference_page(page_index) == 0)
			phymm_free_user(page_index);
	}
	spinlock_unlock(&mm_lock, irq);
}

/* Return the page-table flags (low 12 bits) for the mapping at @vir */
unsigned mm_get_map_flag(vaddr_t vir)
{
	return mm_get_map_flag_pd(mm_get_pagedir(), vir);
}

/* Large-page size is internal; callers see ordinary 4 KiB mapping flags. */
unsigned mm_get_map_flag_pd(vaddr_t page_dir, vaddr_t vir)
{
	mm_addr_info info;

	pte_t pde;

	if (!page_dir)
		return 0;
	pde = ((pte_t *)page_dir)[ADDR_TO_PGT_OFFSET(vir)];

	if (mm_large_entry(pde))
		return pde & (PAGE_SIZE - 1) & ~PAGE_ENTRY_LARGE;
	if (!mm_get_valid_page_table_in_dir((pte_t *)page_dir, vir, &info))
		return 0;
	return *info.entry & ~PAGE_SIZE_MASK;
}

/* Update the page-table flags for the mapping at @vir */
void mm_set_map_flag(vaddr_t vir, unsigned flag)
{
	mm_set_map_flag_pd(mm_get_pagedir(), vir, flag);
}

void mm_set_map_flag_pd(vaddr_t page_dir, vaddr_t vir, unsigned flag)
{
	mm_addr_info info;
	int irq;
	if (!page_dir)
		return;
	spinlock_lock(&mm_lock, &irq);

	if (!mm_split_direct_page((pte_t *)page_dir, vir) ||
	    !mm_get_valid_page_table_in_dir((pte_t *)page_dir, vir, &info))
		goto out;
	*info.entry = (*info.entry & PAGE_SIZE_MASK) | flag;
	if (vir >= KERNEL_OFFSET)
		smp_tlb_flush();
	else
		smp_tlb_flush_user(page_dir);
out:
	spinlock_unlock(&mm_lock, irq);
}

/* Return the physical page index backing the virtual address @vir */
pfn_t mm_get_attached_page_index(vaddr_t vir)
{
	return mm_virt_to_phys(vir) / PAGE_SIZE;
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
vaddr_t vm_alloc_dma(int page_count)
{
	return vm_alloc(page_count);
}

void vm_free(vaddr_t vm, int page_count)
{
	int i;
	int irq;
	unsigned int page_index;

	spinlock_lock(&mm_lock, &irq);
	vm &= PAGE_SIZE_MASK;
	/* Forget page directories before their backing memory can be reused. */
	if (vm >= KERNEL_OFFSET && vm < KERNEL_KMAP_BEGIN)
		for (i = 0; i < page_count; i++) {
			unsigned pfn = (vm - KERNEL_OFFSET) / PAGE_SIZE + i;
			process_dirs[pfn / 32] &= ~(1U << (pfn % 32));
		}
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
void *name_get(void)
{
	char *buf, *region;
	int irq;

	spinlock_lock(&path_lock, &irq);
	if (list_is_empty(&name_cache_head)) {
		spinlock_unlock(&path_lock, irq);
		region = (void *)vm_alloc(1);
		if (!region)
			return NULL;
		buf = region + sizeof(list_entry);
	} else {
		region = (char *)list_remove_tail(&name_cache_head);
		spinlock_unlock(&path_lock, irq);
		buf = region + sizeof(list_entry);
	}
	buf[0] = 0;
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
