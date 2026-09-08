#ifndef MOS_X86_ARCH_MMU_H
#define MOS_X86_ARCH_MMU_H

#include <arch/types.h>
#include <compiler.h>

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
		     : "=&r"(value) : : "memory");
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

#endif
