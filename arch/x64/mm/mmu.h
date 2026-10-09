#ifndef MOS_X64_ARCH_MMU_H
#define MOS_X64_ARCH_MMU_H
#include <arch/types.h>
#include <stddef.h>
#include <compiler.h>
#include <config.h>

/* Four-level AMD64 translation: 9 index bits per level above 4 KiB pages. */
#define MMU_PAGE_SHIFT 12U
#define MMU_TABLE_INDEX_BITS 9U
#define MMU_TABLE_INDEX_MASK ((1U << MMU_TABLE_INDEX_BITS) - 1)
#define MMU_ROOT_SHIFT (MMU_PAGE_SHIFT + 3 * MMU_TABLE_INDEX_BITS)

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
/* Validate the architecture's user range and reserved device window. */
ALWAYS_INLINE int arch_mm_user_range_valid(vaddr_t address, size_t length)
{
	if (address >= MOS_NATIVE_TASK_SIZE ||
	    length > MOS_NATIVE_TASK_SIZE - address)
		return 0;
	if (address < MOS_DEVICE_IO_END &&
	    address + length > MOS_DEVICE_IO_BEGIN)
		return 0;
	return 1;
}

int arch_mm_clone_region(pte_t *src, pte_t *dst, struct _vm_region *region);
pte_t *arch_mm_lookup_leaf(vaddr_t root, vaddr_t address);
void arch_mm_enum_user(vaddr_t root, void (*fn)(void *, vaddr_t, paddr_t),
		       void *aux);
#endif
