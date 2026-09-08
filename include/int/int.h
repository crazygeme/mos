
#ifndef _INT_INT_H
#define _INT_INT_H

#include <config.h>
#include <arch/interrupt.h>
#include <macro.h>
#include <mm/mm.h>

#define INT_M_CTL 0x20
#define INT_M_CTLMASK 0x21
#define INT_S_CTL 0xA0
#define INT_S_CTLMASK 0xA1

#define INT_VECTOR_DIVIDE 0x0
#define INT_VECTOR_DEBUG 0x1
#define INT_VECTOR_NMI 0x2
#define INT_VECTOR_BREAKPOINT 0x3
#define INT_VECTOR_OVERFLOW 0x4
#define INT_VECTOR_BOUNDS 0x5
#define INT_VECTOR_INVAL_OP 0x6
#define INT_VECTOR_COPROC_NOT 0x7
#define INT_VECTOR_DOUBLE_FAULT 0x8
#define INT_VECTOR_COPROC_SEG 0x9
#define INT_VECTOR_INVAL_TSS 0xA
#define INT_VECTOR_SEG_NOT 0xB
#define INT_VECTOR_STACK_FAULT 0xC
#define INT_VECTOR_PROTECTION 0xD
#define INT_VECTOR_PAGE_FAULT 0xE

#define INT_VECTOR_COPROC_ERR 0x10

#define INT_VECTOR_IRQ0 0x20
#define INT_VECTOR_IRQ8 0x28

#define CLOCK_IRQ 0
#define KEYBOARD_IRQ 1

#define DA_386IGate 0x8E

extern void asm_interrupt_handle_for_keyboard();

void int_update_tss(void *address);

void int_enable_all(void);

typedef void (*int_callback)(intr_frame *frame);

void int_register(int vec_no, int_callback fn, int is_trap, int dpl);

void int_unregister(int vec_no);

static __attribute__((always_inline)) inline unsigned int_is_intr_enabled(void)
{
	unsigned flags;
	GET_INTR_FLAG(flags);
	return (flags & 0x00000200) == 0x00000200;
}

static __attribute__((always_inline)) inline unsigned int_intr_enable(void)
{
	unsigned old = int_is_intr_enabled();
	ENABLE_INTR();
	return old;
}

static __attribute__((always_inline)) inline unsigned int_intr_disable(void)
{
	unsigned old = int_is_intr_enabled();
	DISABLE_INTR();
	return old;
}

static __attribute__((always_inline)) inline void
int_intr_setlevel(unsigned enabled)
{
	if (enabled)
		int_intr_enable();
	else
		int_intr_disable();
}

#endif
