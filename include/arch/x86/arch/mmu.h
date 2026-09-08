#ifndef MOS_X86_ARCH_MMU_H
#define MOS_X86_ARCH_MMU_H

unsigned arch_mm_current_address_space(void);
void arch_mm_activate(unsigned address_space);
void arch_mm_flush_local(void);
void arch_mm_invalidate(unsigned address);
unsigned arch_mm_fault_address(void);

#endif
