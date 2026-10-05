#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/mmu.h>
#include <lib/lock.h>

extern spinlock_t mm_lock;

extern short pgc_entry_count[PAGE_TABLE_CACHE_PAGES];

static void copy_one_pte(pte_t *src_pte, pte_t *dst_pte, vm_region *vma,
			 short *pt_count)
{
	pte_t pte = *src_pte;
	paddr_t phy = pte & PAGE_SIZE_MASK;
	unsigned page_index;

	if (!(pte & PAGE_ENTRY_PRESENT))
		return;

	/*
	 * /dev/mem mappings may point at MMIO or reserved physical ranges such
	 * as the linear framebuffer BAR at 0xFD000000.  Those pages are not
	 * owned by phymm, so fork must clone the PTE without taking a RAM page
	 * reference or forcing COW semantics.
	 */
	if (vma->vm_flags & VM_REGION_F_DIRECT_PHYS) {
		if ((*dst_pte & PAGE_SIZE_MASK) == 0)
			(*pt_count)++;
		*dst_pte = pte;
		return;
	}

	page_index = phy / PAGE_SIZE;

	/*
	 * Be defensive for any PTE that points outside allocator-managed RAM or
	 * into a reserved hole.  Those mappings behave like MMIO/firmware pages:
	 * clone the PTE, but never feed the address into phymm reference counts.
	 */
	if (page_index < phymm_begin || page_index >= phymm_end ||
	    phymm_pages[page_index].ref_count == PHYMM_RESERVED) {
		if ((*dst_pte & PAGE_SIZE_MASK) == 0)
			(*pt_count)++;
		*dst_pte = pte;
		return;
	}

	if (!(vma->flag & MAP_SHARED) && (pte & PAGE_ENTRY_WRITABLE)) {
		pte &= ~PAGE_ENTRY_WRITABLE;
		*src_pte = pte; /* write-protect parent */
	}

	if ((*dst_pte & PAGE_SIZE_MASK) == 0)
		(*pt_count)++;

	*dst_pte = pte;
	phymm_reference_page(page_index);
}

/*
 * copy_pte_range — copy all present PTEs within [vma->begin, vma->end)
 * that reside in the page table at src_pd[pde_idx].
 */
static int copy_pte_range(pte_t *src_pd, pte_t *dst_pd, vm_region *vma,
			  unsigned pde_idx)
{
	pte_t *src_pt;
	pte_t *dst_pt;
	vaddr_t pde_base = (vaddr_t)pde_idx << 22;
	vaddr_t pde_end = pde_base + (1u << 22);
	unsigned pt_start =
		(vma->begin > pde_base) ? ADDR_TO_PET_OFFSET(vma->begin) : 0;
	unsigned pt_end =
		(vma->end >= pde_end) ?
			1024 :
			ADDR_TO_PET_OFFSET((vma->end - PAGE_SIZE)) + 1;
	pte_t pde_flag = src_pd[pde_idx] & ~PAGE_SIZE_MASK;
	int cache_idx;
	unsigned i;

	src_pt = (pte_t *)PHY_TO_VIRT(src_pd[pde_idx] & PAGE_SIZE_MASK);

	if (!(dst_pd[pde_idx] & PAGE_SIZE_MASK)) {
		vaddr_t table = mm_alloc_page_table();
		if (!table)
			return 0;
		dst_pd[pde_idx] = VIRT_TO_PHY(table) | pde_flag;
	}

	dst_pt = (pte_t *)PHY_TO_VIRT(dst_pd[pde_idx] & PAGE_SIZE_MASK);
	cache_idx = (PAGE_TABLE_CACHE_END - (uintptr_t)dst_pt) / PAGE_SIZE - 1;

	for (i = pt_start; i < pt_end; i++) {
		if (!(src_pt[i] & PAGE_ENTRY_PRESENT))
			continue;
		copy_one_pte(&src_pt[i], &dst_pt[i], vma,
			     &pgc_entry_count[cache_idx]);
	}
	return 1;
}

/*
 * copy_vma_pages — copy all PTEs for one VMA.
 *
 * Iterates only the PDE indices covered by the VMA; entries that are not
 * present (pages never faulted in) are skipped without descending.
 */
int arch_mm_clone_region(pte_t *src_pd, pte_t *dst_pd, vm_region *vma)
{
	unsigned pde_first = ADDR_TO_PGT_OFFSET(vma->begin);
	unsigned pde_last = ADDR_TO_PGT_OFFSET((vma->end - PAGE_SIZE));
	unsigned pde_idx;
	int irq, result = 1;
	spinlock_lock(&mm_lock, &irq);

	for (pde_idx = pde_first; pde_idx <= pde_last; pde_idx++) {
		if (!(src_pd[pde_idx] & PAGE_SIZE_MASK))
			continue;
		if (!copy_pte_range(src_pd, dst_pd, vma, pde_idx)) {
			result = 0;
			break;
		}
	}
	spinlock_unlock(&mm_lock, irq);
	return result;
}
