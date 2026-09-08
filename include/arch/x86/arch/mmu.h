#ifndef MOS_X86_ARCH_MMU_H
#define MOS_X86_ARCH_MMU_H

#include <arch/types.h>

addr_space_t arch_mm_current_address_space(void);
void arch_mm_activate(addr_space_t address_space);
void arch_mm_flush_local(void);
void arch_mm_invalidate(vaddr_t address);
vaddr_t arch_mm_fault_address(void);

#endif
