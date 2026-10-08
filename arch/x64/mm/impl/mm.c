#include <mm/mm.h>
#include <ps/smp.h>
#include <mm/mmu.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/vdso.h>
#include <lib/klib.h>
#include <lib/lock.h>

static spinlock_t table_lock;

#define ADDRESS_MASK MOS_PTE_ADDRESS_MASK
#define ALIAS_PAGES ((KERNEL_KMAP_END - KERNEL_KMAP_BEGIN) / PAGE_SIZE)
unsigned phymm_begin, phymm_end;
unsigned cache_count, buffer_count, pgc_top;
static vaddr_t table_free[PAGE_TABLE_CACHE_PAGES];
static unsigned table_count;
static pte_t *kernel_root;
static paddr_t alias_physical[ALIAS_PAGES];
static unsigned alias_refs[ALIAS_PAGES];
static spinlock_t mm_lock, path_lock;
static list_entry path_cache;
static int owned(paddr_t physical);

static pte_t *table_pointer(pte_t entry)
{
	paddr_t physical = entry & ADDRESS_MASK;
	return (pte_t *)(physical < KERNEL_DIRECT_MAP_LIMIT ?
				 KERNEL_OFFSET + physical :
				 MOS_PHYS_MAP_BEGIN + physical);
}
static paddr_t table_address(const void *table)
{
	vaddr_t address = (uintptr_t)table;
	return address >= MOS_PHYS_MAP_BEGIN &&
			       address <
				       MOS_PHYS_MAP_BEGIN + MOS_PHYS_MAP_SIZE ?
		       address - MOS_PHYS_MAP_BEGIN :
		       address - KERNEL_OFFSET;
}
vaddr_t mm_get_pagedir(void)
{
	return (vaddr_t)table_pointer(arch_mm_current_address_space());
}
vaddr_t mm_alloc_page_table(void)
{
	int irq;
	vaddr_t result = 0;
	spinlock_lock(&table_lock, &irq);
	if (table_count) {
		result = table_free[--table_count];
		pgc_top = table_count;
		cache_count++;
	}
	spinlock_unlock(&table_lock, irq);
	if (result)
		memset((void *)result, 0, PAGE_SIZE);
	return result;
}
void mm_free_page_table(vaddr_t address)
{
	int irq;
	spinlock_lock(&table_lock, &irq);
	table_free[table_count++] = address;
	pgc_top = table_count;
	cache_count--;
	spinlock_unlock(&table_lock, irq);
}
/* Requires mm_lock. Allocation failure rolls back the tables from this walk. */
static pte_t *ensure_leaf(pte_t *root, vaddr_t address)
{
	pte_t *table = root, *created[3];
	unsigned count = 0;
	if (!root || !arch_mm_is_canonical(address))
		return 0;
	for (unsigned shift = 39; shift > 12; shift -= 9) {
		pte_t *entry = &table[(address >> shift) & 511];
		if (!(*entry & PAGE_ENTRY_PRESENT)) {
			vaddr_t child = mm_alloc_page_table();
			if (!child)
				goto failed;
			*entry = table_address((void *)child) |
				 PAGE_ENTRY_PAGE_TABLE |
				 (address < MOS_NATIVE_TASK_SIZE ?
					  PAGE_ENTRY_DPL_USER :
					  0);
			created[count++] = entry;
		} else if (*entry & PAGE_ENTRY_LARGE) {
			/* Bootstrap direct map uses 2 MiB leaves. Kernel ancestors
			 * are shared, so a split is visible in every address space. */
			if (shift != 21 || address < MOS_NATIVE_TASK_SIZE)
				goto failed;
			vaddr_t child = mm_alloc_page_table();
			if (!child)
				goto failed;
			pte_t old = *entry;
			pte_t *leaves = (pte_t *)child;
			paddr_t base = (old & ADDRESS_MASK) &
				       ~(0x200000ULL - 1);
			for (unsigned i = 0; i < 512; i++)
				leaves[i] = (base + i * PAGE_SIZE) |
					    (old & ~ADDRESS_MASK &
					     ~PAGE_ENTRY_LARGE);
			*entry = table_address(leaves) | PAGE_ENTRY_PAGE_TABLE;
			arch_cpu_reload_tlb();
		}
		table = table_pointer(*entry);
	}
	return &table[(address >> 12) & 511];
failed:
	while (count) {
		pte_t *entry = created[--count];
		mm_free_page_table((vaddr_t)table_pointer(*entry));
		*entry = 0;
	}
	return 0;
}
paddr_t mm_virt_to_phys(vaddr_t address)
{
	pte_t *table = (pte_t *)mm_get_pagedir();
	if (!arch_mm_is_canonical(address))
		return 0;
	for (unsigned shift = 39; shift >= 12; shift -= 9) {
		pte_t entry = table[(address >> shift) & 511];
		if (!(entry & PAGE_ENTRY_PRESENT))
			return 0;
		if (shift == 12 || (entry & PAGE_ENTRY_LARGE)) {
			paddr_t mask = (1ULL << shift) - 1;
			return ((entry & ADDRESS_MASK) & ~mask) |
			       (address & mask);
		}
		table = table_pointer(entry);
	}
	return 0;
}
void mm_init_cache(void)
{
	spinlock_init(&table_lock);
	table_count = PAGE_TABLE_CACHE_PAGES;
	for (unsigned i = 0; i < table_count; i++)
		table_free[i] = PAGE_TABLE_CACHE_BEGIN + i * PAGE_SIZE;
	kernel_root = (pte_t *)mm_get_pagedir();
	pte_t *pdpt = table_pointer(kernel_root[511]);
	pte_t *pd = table_pointer(pdpt[511]);
	for (unsigned i = 0; i < 512; i++)
		pd[i] = i < KERNEL_DIRECT_MAP_LIMIT / 0x200000 ?
				pd[i] | PAGE_ENTRY_GLOBAL :
				0;
	kernel_root[0] = table_address((void *)mm_alloc_page_table()) |
			 PAGE_ENTRY_PAGE_TABLE;
	table_pointer(kernel_root[0])[3] =
		table_address((void *)mm_alloc_page_table()) |
		PAGE_ENTRY_PAGE_TABLE;
	/* The 64-bit address space can retain all allocator RAM permanently.
	 * Managed high pages no longer require temporary, globally invalidated
	 * aliases for every COW copy. Device/firmware mappings keep their APIs. */
	pte_t *ram_pdpt = (pte_t *)mm_alloc_page_table();
	kernel_root[(MOS_PHYS_MAP_BEGIN >> 39) & 511] =
		table_address(ram_pdpt) | PAGE_ENTRY_PAGE_TABLE;
	for (unsigned gigabyte = 0; gigabyte < MOS_PHYS_MAP_SIZE >> 30;
	     gigabyte++) {
		pte_t *ram_pd = (pte_t *)mm_alloc_page_table();
		ram_pdpt[gigabyte] = table_address(ram_pd) |
				     PAGE_ENTRY_PAGE_TABLE;
		for (unsigned i = 0; i < 512; i++)
			ram_pd[i] = ((paddr_t)gigabyte << 30) |
				    ((paddr_t)i << 21) |
				    PAGE_ENTRY_KERNEL_DATA | PAGE_ENTRY_LARGE |
				    (1ULL << 63);
	}
	spinlock_init(&mm_lock);
	spinlock_init(&path_lock);
	list_init(&path_cache);
	arch_mm_flush_local();
}
void mm_init_process_page_dir(vaddr_t root)
{
	pte_t *dst = (pte_t *)root;
	memset(dst, 0, PAGE_SIZE);
	memcpy(dst + 256, kernel_root + 256, 256 * sizeof(pte_t));
	vaddr_t lower = mm_alloc_page_table();
	if (!lower)
		return;
	dst[0] = table_address((void *)lower) | PAGE_ENTRY_PAGE_TABLE |
		 PAGE_ENTRY_DPL_USER;
	((pte_t *)lower)[3] = table_pointer(kernel_root[0])[3];
}
void mm_del_user_map(void)
{
	/* Bootstrap low mappings were removed in mm_init_cache. */
	arch_mm_flush_local();
	arch_mm_enable_global_pages();
}
int mm_kmap_page(vaddr_t address)
{
	return address >= KERNEL_OFFSET && address < KERNEL_KMAP_BEGIN ? 1 : -1;
}
void mm_kunmap_page(vaddr_t address)
{
	(void)address;
}
int mm_kmap_phys(paddr_t physical)
{
	paddr_t page = physical & ADDRESS_MASK;
	unsigned free_slot = ALIAS_PAGES;
	int irq;
	if (page < KERNEL_DIRECT_MAP_LIMIT ||
	    (page < MOS_PHYS_MAP_SIZE && owned(page)))
		return 1;
	spinlock_lock(&mm_lock, &irq);
	for (unsigned i = 0; i < ALIAS_PAGES; i++) {
		if (alias_refs[i] && alias_physical[i] == page) {
			alias_refs[i]++;
			spinlock_unlock(&mm_lock, irq);
			return 1;
		}
		if (!alias_refs[i] && free_slot == ALIAS_PAGES)
			free_slot = i;
	}
	if (free_slot != ALIAS_PAGES) {
		vaddr_t address = KERNEL_KMAP_BEGIN + free_slot * PAGE_SIZE;
		pte_t *entry = ensure_leaf(kernel_root, address);
		if (entry) {
			*entry = page | PAGE_ENTRY_KERNEL_DATA;
			alias_physical[free_slot] = page;
			alias_refs[free_slot] = 1;
			smp_tlb_flush();
			spinlock_unlock(&mm_lock, irq);
			return 1;
		}
	}
	spinlock_unlock(&mm_lock, irq);
	return -1;
}
vaddr_t mm_phys_to_virt(paddr_t physical)
{
	paddr_t page = physical & ADDRESS_MASK;
	if (page < KERNEL_DIRECT_MAP_LIMIT)
		return KERNEL_OFFSET + physical;
	if (page < MOS_PHYS_MAP_SIZE && owned(page))
		return MOS_PHYS_MAP_BEGIN + physical;
	for (unsigned i = 0; i < ALIAS_PAGES; i++)
		if (alias_refs[i] && alias_physical[i] == page)
			return KERNEL_KMAP_BEGIN + i * PAGE_SIZE +
			       (physical & (PAGE_SIZE - 1));
	return 0;
}
void mm_kunmap_phys(paddr_t physical)
{
	paddr_t page = physical & ADDRESS_MASK;
	int irq;
	if (page < KERNEL_DIRECT_MAP_LIMIT ||
	    (page < MOS_PHYS_MAP_SIZE && owned(page)))
		return;
	spinlock_lock(&mm_lock, &irq);
	for (unsigned i = 0; i < ALIAS_PAGES; i++) {
		if (!alias_refs[i] || alias_physical[i] != page)
			continue;
		if (--alias_refs[i] == 0) {
			vaddr_t address = KERNEL_KMAP_BEGIN + i * PAGE_SIZE;
			pte_t *entry = arch_mm_lookup_leaf((vaddr_t)kernel_root,
							   address);
			*entry = 0;
			smp_tlb_flush();
		}
		break;
	}
	spinlock_unlock(&mm_lock, irq);
}
int mm_copy_phys_page(paddr_t dst, paddr_t src)
{
	if ((dst | src) & (PAGE_SIZE - 1))
		return 0;
	if (mm_kmap_phys(src) != 1)
		return 0;
	if (mm_kmap_phys(dst) != 1) {
		mm_kunmap_phys(src);
		return 0;
	}
	memcpy((void *)PHY_TO_VIRT(dst), (void *)PHY_TO_VIRT(src), PAGE_SIZE);
	mm_kunmap_phys(dst);
	mm_kunmap_phys(src);
	return 1;
}
/* Map device resources at their physical addresses, as the existing device
 * API requires. This is a supervisor-only lower-half mapping, replicated at
 * the PDPT level in each process; it never grants userspace access. */
int mm_map_io(paddr_t physical)
{
	if (physical < DEVICE_IO_BEGIN || physical >= DEVICE_IO_END)
		return -1;
	int irq, result = -1;
	spinlock_lock(&mm_lock, &irq);
	pte_t *entry = ensure_leaf(kernel_root, physical);
	if (!entry)
		goto out;
	*entry = (physical & ADDRESS_MASK) | PAGE_ENTRY_KERNEL_DATA |
		 PAGE_ENTRY_CD | PAGE_ENTRY_WT;
	smp_tlb_flush();
	result = 1;
out:
	spinlock_unlock(&mm_lock, irq);
	return result;
}
static pte_t encode_flags(unsigned flags)
{
	return (flags & ~PAGE_ENTRY_NO_EXEC) |
	       ((flags & PAGE_ENTRY_NO_EXEC) ? 1ULL << 63 : 0);
}
static void flush_mapping(vaddr_t root, vaddr_t address)
{
	if (address < MOS_NATIVE_TASK_SIZE &&
	    (address < MOS_COMPAT_TASK_SIZE || address >= 0x100000000ULL))
		smp_tlb_flush_user(root);
	else
		smp_tlb_flush();
}
static int install(vaddr_t address, paddr_t physical, unsigned flags)
{
	pte_t *entry = ensure_leaf((pte_t *)mm_get_pagedir(), address);
	if (!entry)
		return -1;
	pte_t old = *entry;
	*entry = (physical & ADDRESS_MASK) | encode_flags(flags);
	/* Non-present entries have no cached translation. Replacements still
	 * need synchronous invalidation before an old frame can be reused. */
	if (old & PAGE_ENTRY_PRESENT)
		flush_mapping(mm_get_pagedir(), address);
	return 1;
}
int mm_map_page_io(vaddr_t address, paddr_t physical, unsigned flags)
{
	int irq;
	spinlock_lock(&mm_lock, &irq);
	int result = install(address, physical, flags);
	spinlock_unlock(&mm_lock, irq);
	return result;
}
int mm_map_page(vaddr_t address, paddr_t physical, unsigned flags)
{
	int allocated = !physical;
	unsigned page;
	if (allocated) {
		page = phymm_alloc_user();
		if (page == PHYMM_INVALID) {
			phymm_reclaim_user_cache(32);
			page = phymm_alloc_user();
			if (page == PHYMM_INVALID)
				return -1;
		}
		physical = (paddr_t)page * PAGE_SIZE;
	} else
		page = physical / PAGE_SIZE;
	int result = mm_map_page_io(address, physical, flags);
	if (result == 1)
		phymm_reference_page(page);
	else if (allocated)
		phymm_free_user(page);
	return result;
}
static int owned(paddr_t physical)
{
	unsigned page = physical / PAGE_SIZE;
	return page >= phymm_begin && page < phymm_end &&
	       phymm_pages[page].ref_count != PHYMM_RESERVED;
}
static void release(pte_t entry)
{
	if (entry & PAGE_ENTRY_DIRECT_PHYS)
		return;
	paddr_t physical = entry & ADDRESS_MASK;
	unsigned page = physical / PAGE_SIZE;
	if ((owned(physical) || mm_vdso_region(physical)) &&
	    phymm_is_used(page) && phymm_dereference_page(page) == 0)
		phymm_free_user(page);
}
/* Reclaim empty private tables after every CPU has discarded their entries.
 * Shared upper-half ancestors and the shared device PD remain permanent. */
static void prune_user_tables(vaddr_t address)
{
	if (address >= MOS_NATIVE_TASK_SIZE ||
	    (address >= MOS_COMPAT_TASK_SIZE && address < 0x100000000ULL))
		return;
	pte_t *parents[3], *tables[3], *table = (pte_t *)mm_get_pagedir();
	unsigned count = 0;
	for (unsigned shift = 39; shift > 12; shift -= 9) {
		pte_t *entry = &table[(address >> shift) & 511];
		if (!(*entry & PAGE_ENTRY_PRESENT) ||
		    (*entry & PAGE_ENTRY_LARGE))
			return;
		parents[count] = entry;
		tables[count] = table_pointer(*entry);
		table = tables[count++];
	}
	while (count) {
		unsigned i = --count, empty = 1;
		for (unsigned j = 0; j < 512; j++)
			if (tables[i][j] & PAGE_ENTRY_PRESENT) {
				empty = 0;
				break;
			}
		if (!empty)
			break;
		*parents[i] = 0;
		flush_mapping(mm_get_pagedir(), address);
		mm_free_page_table((vaddr_t)tables[i]);
	}
}
void mm_unmap_page(vaddr_t address)
{
	int irq;
	spinlock_lock(&mm_lock, &irq);
	pte_t *entry = arch_mm_lookup_leaf(mm_get_pagedir(), address);
	if (entry && (*entry & PAGE_ENTRY_PRESENT)) {
		pte_t old = *entry;
		*entry = 0;
		flush_mapping(mm_get_pagedir(), address);
		release(old);
		prune_user_tables(address);
	}
	spinlock_unlock(&mm_lock, irq);
}
static void destroy(pte_t *table, unsigned shift, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		pte_t entry = table[i];
		if (shift == 30 && entry == table_pointer(kernel_root[0])[3])
			continue;
		if (!(entry & PAGE_ENTRY_PRESENT))
			continue;
		table[i] = 0;
		if (shift == 12)
			release(entry);
		else if (!(entry & PAGE_ENTRY_LARGE)) {
			pte_t *child = table_pointer(entry);
			destroy(child, shift - 9, 512);
			mm_free_page_table((vaddr_t)child);
		}
	}
}
void mm_destroy_user_map(vaddr_t root)
{
	int irq;
	spinlock_lock(&mm_lock, &irq);
	if (root)
		destroy((pte_t *)root, 39, 256);
	spinlock_unlock(&mm_lock, irq);
}
unsigned mm_get_map_flag_pd(vaddr_t root, vaddr_t address)
{
	pte_t *table = (pte_t *)root;
	if (!table || !arch_mm_is_canonical(address))
		return 0;
	for (unsigned shift = 39; shift >= 12; shift -= 9) {
		pte_t entry = table[(address >> shift) & 511];
		if (!(entry & PAGE_ENTRY_PRESENT))
			return 0;
		if (shift == 12 || (entry & PAGE_ENTRY_LARGE)) {
			unsigned flags = entry & 0xfff;
			/* Large-page size is a hardware layout bit, not a caller
			 * permission. Do not carry it into a split 4 KiB PTE. */
			if (shift != 12)
				flags &= ~PAGE_ENTRY_LARGE;
			return flags | ((entry >> 63) ? PAGE_ENTRY_NO_EXEC : 0);
		}
		table = table_pointer(entry);
	}
	return 0;
}
unsigned mm_get_map_flag(vaddr_t address)
{
	return mm_get_map_flag_pd(mm_get_pagedir(), address);
}
void mm_set_map_flag_pd(vaddr_t root, vaddr_t address, unsigned flags)
{
	int irq;
	spinlock_lock(&mm_lock, &irq);
	pte_t *entry = ensure_leaf((pte_t *)root, address);
	if (!entry || !(*entry & PAGE_ENTRY_PRESENT))
		goto out;
	*entry = (*entry & ADDRESS_MASK) | encode_flags(flags);
	flush_mapping(root, address);
out:
	spinlock_unlock(&mm_lock, irq);
}
void mm_set_map_flag(vaddr_t address, unsigned flags)
{
	mm_set_map_flag_pd(mm_get_pagedir(), address, flags);
}
pfn_t mm_get_attached_page_index(vaddr_t address)
{
	return mm_virt_to_phys(address) / PAGE_SIZE;
}
static vaddr_t vm_alloc_pool(int count, int dma)
{
	if (count <= 0)
		return 0;
	unsigned page = dma ? phymm_alloc_dma(count) :
			      phymm_alloc_kernel(count);
	if (page == PHYMM_INVALID) {
		phymm_reclaim_kernel_cache(32);
		page = dma ? phymm_alloc_dma(count) : phymm_alloc_kernel(count);
		if (page == PHYMM_INVALID)
			return 0;
	}
	for (int i = 0; i < count; i++)
		phymm_reference_page(page + i);
	buffer_count += count;
	return PHY_TO_VIRT((paddr_t)page * PAGE_SIZE);
}
vaddr_t vm_alloc(int count)
{
	return vm_alloc_pool(count, 0);
}
vaddr_t vm_alloc_dma(int count)
{
	return vm_alloc_pool(count, 1);
}
void vm_free(vaddr_t address, int count)
{
	unsigned page = VIRT_TO_PHY(address) / PAGE_SIZE;
	for (int i = 0; i < count; i++)
		phymm_dereference_page(page + i);
	phymm_free_kernel(page, count);
	buffer_count -= count;
}
void *name_get(void)
{
	int irq;
	spinlock_lock(&path_lock, &irq);
	void *region =
		list_is_empty(&path_cache) ? 0 : list_remove_tail(&path_cache);
	spinlock_unlock(&path_lock, irq);
	if (!region)
		region = (void *)vm_alloc(1);
	if (!region)
		return 0;
	void *buffer = (char *)region + sizeof(list_entry);
	*(char *)buffer = 0;
	cache_count++;
	return buffer;
}

void name_put(void *buffer)
{
	if (!buffer)
		return;
	int irq;
	spinlock_lock(&path_lock, &irq);
	list_insert_tail(&path_cache,
			 (list_entry *)((char *)buffer - sizeof(list_entry)));
	spinlock_unlock(&path_lock, irq);
	cache_count--;
}
int arch_mm_clone_region(pte_t *src, pte_t *dst, vm_region *region)
{
	int irq, result = 1;
	spinlock_lock(&mm_lock, &irq);
	for (vaddr_t address = region->begin; address < region->end;
	     address += PAGE_SIZE) {
		pte_t *parent = arch_mm_lookup_leaf((vaddr_t)src, address);
		if (!parent || !(*parent & PAGE_ENTRY_PRESENT))
			continue;
		pte_t *child = ensure_leaf(dst, address);
		if (!child) {
			result = 0;
			break;
		}
		pte_t entry = *parent;
		int managed = !(region->vm_flags & VM_REGION_F_DIRECT_PHYS) &&
			      owned(entry & ADDRESS_MASK);
		if (managed && !(region->flag & MAP_SHARED)) {
			entry &= ~PAGE_ENTRY_WRITABLE;
			*parent = entry;
		}
		*child = entry;
		if (managed || mm_vdso_region(entry & ADDRESS_MASK))
			phymm_reference_page((entry & ADDRESS_MASK) /
					     PAGE_SIZE);
	}
	spinlock_unlock(&mm_lock, irq);
	return result;
}
