#include <arch/mmu.h>

addr_space_t arch_mm_current_address_space(void)
{
	addr_space_t value;
	asm volatile("movl %%cr3, %0" : "=r"(value));
	return value;
}

void arch_mm_activate(addr_space_t address_space)
{
	asm volatile("movl %0, %%cr3" : : "r"(address_space) : "memory");
}

void arch_mm_flush_local(void)
{
	unsigned value;
	asm volatile("movl %%cr3, %0; movl %0, %%cr3"
		     : "=&r"(value) : : "memory");
}

void arch_mm_invalidate(vaddr_t address)
{
	asm volatile("invlpg (%0)" : : "r"(address) : "memory");
}

vaddr_t arch_mm_fault_address(void)
{
	vaddr_t value;
	asm volatile("movl %%cr2, %0" : "=r"(value));
	return value;
}
