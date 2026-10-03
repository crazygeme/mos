#include <arch/config.h>
#include <mm/mm.h>
#include <mm/mmu.h>
#include <mm/mmap.h>
#include <mm/phymm.h>

/* Page-table pointers returned by PHY_TO_VIRT are kernel virtual addresses;
 * entries always contain physical addresses and independent flag bits. */
pte_t *arch_mm_lookup_leaf(vaddr_t root, vaddr_t address)
{
	pte_t *table = (pte_t *)root;

	if (!table || !arch_mm_is_canonical(address))
		return 0;
	for (unsigned shift = 39; shift > 12; shift -= 9) {
		pte_t entry = table[(address >> shift) & 511];
		if (!(entry & PAGE_ENTRY_PRESENT) || (entry & PAGE_ENTRY_LARGE))
			return 0;
		table = (pte_t *)PHY_TO_VIRT(arch_mm_entry_address(entry));
		if (!table)
			return 0;
	}
	return &table[(address >> 12) & 511];
}

static void enum_table(pte_t *table, unsigned shift, vaddr_t base,
		       unsigned count, void (*fn)(void *, vaddr_t, paddr_t),
		       void *aux)
{
	for (unsigned i = 0; i < count; i++) {
		pte_t entry = table[i];
		vaddr_t address = base | ((vaddr_t)i << shift);
		if (shift == 30 && address == 0xc0000000ULL)
			continue;
		if (!(entry & PAGE_ENTRY_PRESENT))
			continue;
		if (shift == 12) {
			fn(aux, address, arch_mm_entry_address(entry));
		} else if (entry & PAGE_ENTRY_LARGE) {
			/* Export ordinary page granularity even for 1 GiB or 2 MiB
			 * leaves, so callers never depend on the hardware leaf size. */
			paddr_t physical = arch_mm_entry_address(entry) &
					   ~((1ULL << shift) - 1);
			for (vaddr_t offset = 0; offset < (1ULL << shift);
			     offset += PAGE_SIZE)
				fn(aux, address + offset, physical + offset);
		} else {
			pte_t *child = (pte_t *)PHY_TO_VIRT(
				arch_mm_entry_address(entry));
			if (child)
				enum_table(child, shift - 9, address, 512, fn,
					   aux);
		}
	}
}

void arch_mm_enum_user(vaddr_t root, void (*fn)(void *, vaddr_t, paddr_t),
		       void *aux)
{
	if (root && fn)
		enum_table((pte_t *)root, 39, 0, 256, fn, aux);
}
