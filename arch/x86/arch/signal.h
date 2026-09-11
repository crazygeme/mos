#ifndef MOS_X86_ARCH_SIGNAL_H
#define MOS_X86_ARCH_SIGNAL_H

#include <arch/types.h>

/*
 * Linux/i386 legacy signal frame placed on the user stack by signal delivery.
 * The restorer returns through trampoline[]; sigreturn recovers this frame
 * after the handler has consumed the return address and signal argument.
 */
typedef struct _arch_signal_frame {
	arch_reg_t return_addr;
	int signo;
	arch_reg_t saved_eip;
	unsigned int saved_eflags;
	arch_reg_t saved_esp;
	unsigned int saved_eax;
	unsigned int saved_ebx;
	unsigned int saved_ecx;
	unsigned int saved_edx;
	unsigned int saved_esi;
	unsigned int saved_edi;
	unsigned int saved_ebp;
	unsigned int saved_ds;
	unsigned int saved_es;
	unsigned int saved_fs;
	unsigned int saved_gs;
	unsigned long saved_mask;
	unsigned char trampoline[8];
} arch_signal_frame;

typedef arch_signal_frame signal_frame;

#endif
