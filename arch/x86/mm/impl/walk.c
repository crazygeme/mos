#include <mm/mm.h>
#include <mm/mmu.h>
#include <ps/ps.h>

void arch_mm_enum_user(vaddr_t root, fpuser_map_callback fn, void *aux)
{
	unsigned i, j;
	pte_t *page_dir;

	if (!fn || !root)
		return;

	page_dir = (pte_t *)root;
	for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
		pte_t *page_table = (pte_t *)(page_dir[i] & PAGE_SIZE_MASK);
		if (!page_table)
			continue;
		page_table = (pte_t *)PHY_TO_VIRT((paddr_t)page_table);
		for (j = 0; j < 1024; j++) {
			if ((page_table[j] & PAGE_SIZE_MASK) == 0)
				continue;
			vaddr_t vir = ((vaddr_t)i << 22) + (j << 12);
			paddr_t phy = page_table[j] & PAGE_SIZE_MASK;
			fn(aux, vir, phy);
		}
	}
}

pte_t *arch_mm_lookup_leaf(vaddr_t root, vaddr_t address)
{
	pte_t *dir = (pte_t *)root;
	pte_t pde;
	if (!dir)
		return 0;
	pde = dir[ADDR_TO_PGT_OFFSET(address)];
	if (!(pde & PAGE_ENTRY_PRESENT) || (pde & PAGE_ENTRY_LARGE))
		return 0;
	return &((pte_t *)PHY_TO_VIRT(
		pde & PAGE_SIZE_MASK))[ADDR_TO_PET_OFFSET(address)];
}
