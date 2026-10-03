#ifndef MOS_X64_ARCH_MMU_H
#define MOS_X64_ARCH_MMU_H
#include <arch/types.h>
#include <compiler.h>

ALWAYS_INLINE addr_space_t arch_mm_current_address_space(void)
{
	addr_space_t root;
	asm volatile("mov %%cr3, %0" : "=r"(root));
	return root;
}
ALWAYS_INLINE void arch_mm_activate(addr_space_t root)
{
	asm volatile("mov %0, %%cr3" : : "r"(root) : "memory");
}
ALWAYS_INLINE void arch_mm_flush_local(void)
{
	arch_mm_activate(arch_mm_current_address_space());
}
ALWAYS_INLINE void arch_mm_invalidate(vaddr_t address)
{
	asm volatile("invlpg (%0)" : : "r"(address) : "memory");
}
ALWAYS_INLINE vaddr_t arch_mm_fault_address(void)
{
	vaddr_t address;
	asm volatile("mov %%cr2, %0" : "=r"(address));
	return address;
}
ALWAYS_INLINE void arch_mm_enable_global_pages(void)
{
	unsigned long cr4;
	asm volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= 1UL << 7;
	asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}
ALWAYS_INLINE int arch_mm_is_canonical(vaddr_t address)
{
	return (uint64_t)((int64_t)(address << 16) >> 16) == address;
}
ALWAYS_INLINE paddr_t arch_mm_entry_address(pte_t entry)
{
	return entry & 0x000ffffffffff000ULL;
}
struct _vm_region;
int arch_mm_clone_region(pte_t *src, pte_t *dst, struct _vm_region *region);
pte_t *arch_mm_lookup_leaf(vaddr_t root, vaddr_t address);
void arch_mm_enum_user(vaddr_t root, void (*fn)(void *, vaddr_t, paddr_t),
		       void *aux);
#endif
