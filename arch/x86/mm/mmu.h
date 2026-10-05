#ifndef MOS_X86_ARCH_MMU_H
#define MOS_X86_ARCH_MMU_H

#include <arch/types.h>
#include <stddef.h>
#include <compiler.h>
#include <config.h>

ALWAYS_INLINE addr_space_t arch_mm_current_address_space(void)
{
	addr_space_t value;
	asm volatile("movl %%cr3, %0" : "=r"(value));
	return value;
}

ALWAYS_INLINE void arch_mm_activate(addr_space_t address_space)
{
	asm volatile("movl %0, %%cr3" : : "r"(address_space) : "memory");
}

ALWAYS_INLINE void arch_mm_flush_local(void)
{
	addr_space_t value;
	asm volatile("movl %%cr3, %0; movl %0, %%cr3"
		     : "=&r"(value)
		     :
		     : "memory");
}

/* Non-PAE x86 large pages are 4 MiB leaves in the page directory. */
ALWAYS_INLINE int arch_mm_enable_large_pages(void)
{
	unsigned a, b, c, d, value;

	asm volatile("cpuid"
		     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
		     : "a"(1), "c"(0));
	if (!(d & (1U << 3))) /* CPUID.1:EDX.PSE */
		return 0;
	asm volatile("movl %%cr4, %0" : "=r"(value));
	value |= 1U << 4; /* CR4.PSE */
	asm volatile("movl %0, %%cr4" : : "r"(value) : "memory");
	return 1;
}

ALWAYS_INLINE void arch_mm_enable_global_pages(void)
{
	unsigned long value;

	asm volatile("movl %%cr4, %0" : "=r"(value));
	value |= 1UL << 7; /* CR4.PGE */
	asm volatile("movl %0, %%cr4" : : "r"(value) : "memory");
}

ALWAYS_INLINE void arch_mm_invalidate(vaddr_t address)
{
	asm volatile("invlpg (%0)" : : "r"(address) : "memory");
}

ALWAYS_INLINE vaddr_t arch_mm_fault_address(void)
{
	vaddr_t value;
	asm volatile("movl %%cr2, %0" : "=r"(value));
	return value;
}

ALWAYS_INLINE paddr_t arch_mm_entry_address(pte_t entry)
{
	return entry & PAGE_SIZE_MASK;
}

struct _vm_region;
/* Validate the architecture's user range and reserved device window. */
ALWAYS_INLINE int arch_mm_user_range_valid(vaddr_t address, size_t length)
{
	if (address >= MOS_NATIVE_TASK_SIZE ||
	    length > MOS_NATIVE_TASK_SIZE - address)
		return 0;
	return 1;
}

int arch_mm_clone_region(pte_t *src, pte_t *dst, struct _vm_region *region);
pte_t *arch_mm_lookup_leaf(vaddr_t root, vaddr_t address);
void arch_mm_enum_user(vaddr_t root, void (*fn)(void *, vaddr_t, paddr_t),
		       void *aux);

#endif
