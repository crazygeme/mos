/*
 * ap_trampoline.S — Application Processor startup trampoline.
 *
 * The BSP copies this code to physical AP_TRAMPOLINE_PHYS (0x8000) and
 * sends SIPI with vector 0x08 (page 8 = 0x8000).  Each AP starts here
 * in 16-bit real mode.
 *
 * Sequence:
 *   1. 16-bit real mode: load temporary GDT, enable protected mode.
 *   2. 32-bit protected mode (no paging): set segments, load CR3.
 *   3. Enable paging (BSP has restored identity mapping for 0–4 MB).
 *   4. Far-jump to ap_trampoline_vmode (high virtual address in kernel).
 *   5. ap_trampoline_vmode: reload BSP GDT/IDT, set up stack, call C init.
 *
 * AP params page at physical AP_PARAMS_PHYS (0x9000):
 *   [+0]  uint32  cr3         — BSP page directory physical address
 *   [+4]  uint32  stack_top   — per-AP kernel stack virtual address
 *   [+8]  uint32  init_fn     — virtual address of ap_init_c()
 *   [+12] uint32  cpu_id      — logical CPU id for this AP
 */

#include <config.h>

#ifndef KERNEL_OFFSET
#define KERNEL_OFFSET 0xC0000000
#endif

/* Physical address where the trampoline will be placed at runtime. */
#define TRAMPOLINE_BASE  AP_TRAMPOLINE_PHYS

/* Physical address of the AP params page. */
#define PARAMS_BASE      AP_PARAMS_PHYS

/*
 * Temp GDT selector layout mirrors the BSP GDT (stage1.c setup_gdt):
 *   KERNEL_DATA_SELECTOR = 0x08  (entry 1, data read/write)
 *   KERNEL_CODE_SELECTOR = 0x10  (entry 2, code execute/read)
 */
#define TMP_GDT_CODE_SEL  0x10   /* code descriptor at index 2 */
#define TMP_GDT_DATA_SEL  0x08   /* data descriptor at index 1 */

/*
 * 16-bit real-mode entry — executed at physical TRAMPOLINE_BASE
 */
.section .text.trampoline, "ax"
.code16

.global ap_trampoline_start
ap_trampoline_start:
        cli
        cld
        xorw    %ax, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %ss

        /*
         * Load the temporary 3-entry GDT (embedded at ap_tmp_gdtdesc).
         * The descriptor holds a physical base address, computable as:
         *   TRAMPOLINE_BASE + (ap_tmp_gdt - ap_trampoline_start)
         * The assembler resolves the label difference to a constant.
         */
        .byte   0x0f, 0x01, 0x16        /* LGDT r/m16 */
        .word   TRAMPOLINE_BASE + (ap_tmp_gdtdesc - ap_trampoline_start)

        /* Enable protected mode (CR0.PE). */
        movl    %cr0, %eax
        orl     $0x1, %eax
        movl    %eax, %cr0

        /*
         * Far jump into 32-bit protected mode.
         * We emit the ljmp opcode manually with an operand-size prefix
         * (0x66) so the EIP field is 32 bits wide.
         * Target physical address = TRAMPOLINE_BASE + offset of ap_pm32.
         */
        .byte   0x66, 0xea
        .long   TRAMPOLINE_BASE + (ap_pm32 - ap_trampoline_start)
        .word   TMP_GDT_CODE_SEL

/*
 * 32-bit protected mode — paging NOT yet enabled
 */
.code32
.align  4
ap_pm32:
        movw    $TMP_GDT_DATA_SEL, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %fs
        movw    %ax, %gs
        movw    %ax, %ss

        /* Load CR3 from params page (physical address). */
        movl    PARAMS_BASE + 0, %eax
        movl    %eax, %cr3

        /* Enable paging (CR0.PG).
         * The BSP has restored the identity mapping for virt[0..4MB]
         * so execution continues uninterrupted at the current EIP. */
        movl    %cr0, %eax
        orl     $0x80000000, %eax
        movl    %eax, %cr0

        /*
         * Far jump to ap_trampoline_vmode — a label in the kernel image,
         * linked at its true HIGH virtual address (e.g. 0xC0xxxxxx).
         * This atomically raises EIP into the mapped kernel region.
         * The selector KERNEL_CODE_SELECTOR (0x10) must exist in the
         * temporary GDT — and it does (TMP_GDT_CODE_SEL == 0x08, but we
         * need 0x10 for KERNEL_CODE_SELECTOR).  We reuse entry 2.
         *
         * NOTE: KERNEL_CODE_SELECTOR == 0x10 == TMP_GDT_CODE_SEL + 8.
         * We placed the code descriptor at GDT index 2 (sel 0x10) and
         * the data descriptor at index 1 (sel 0x08) to match the BSP's
         * GDT layout.  See ap_tmp_gdt below.
         */
        ljmp    $KERNEL_CODE_SELECTOR, $ap_trampoline_vmode

/*
 * High-virtual-mode continuation — paging enabled, running at kernel VA.
 */
.global ap_trampoline_vmode
ap_trampoline_vmode:
        /* Set up the per-AP kernel stack FIRST — ESP is undefined at this
         * point (SIPI resets it to 0).  All subsequent code uses the stack
         * (LGDT/LIDT build their descriptor on it), so this must come first. */
        movl    (PARAMS_BASE + KERNEL_OFFSET + 4), %esp
        movl    %esp, %ebp

        /* Reload the BSP's real GDT (gdt[] in stage1.c).
         * gdt_size and idt_size are unsigned short; use movzwl to
         * zero-extend the 16-bit variable into a 32-bit register. */
        subl    $8, %esp
        movzwl  gdt_size, %eax
        decl    %eax
        movw    %ax, (%esp)             /* limit */
        movl    $gdt, %eax
        movl    %eax, 2(%esp)           /* base (virtual) */
        lgdt    (%esp)
        addl    $8, %esp

        /* Reload the IDT (idt[] in stage1.c). */
        subl    $8, %esp
        movzwl  idt_size, %eax
        decl    %eax
        movw    %ax, (%esp)
        movl    $idt, %eax
        movl    %eax, 2(%esp)
        lidt    (%esp)
        addl    $8, %esp

        /* Reload segment registers from the BSP GDT. */
        movw    $KERNEL_DATA_SELECTOR, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %fs
        movw    %ax, %gs
        movw    %ax, %ss

        /* Pass cpu_id as the first argument, then call ap_init_c(). */
        movl    (PARAMS_BASE + KERNEL_OFFSET + 12), %eax   /* cpu_id */
        pushl   %eax
        movl    (PARAMS_BASE + KERNEL_OFFSET + 8), %eax    /* init_fn */
        call    *%eax
        addl    $4, %esp

        /* Should never reach here. */
        cli
1:      hlt
        jmp     1b

/*
 * Temporary GDT (3 entries; same selector layout as the BSP GDT)
 *   [0] null
 *   [1] kernel data  (sel 0x08) — flat 32-bit R/W
 *   [2] kernel code  (sel 0x10) — flat 32-bit R/X
 */
.align  4
ap_tmp_gdt:
        .quad   0x0000000000000000      /* 0x00: null */
        /* 0x08: data (base=0, limit=4GB, DPL=0, type=data r/w) */
        .word   0xFFFF, 0x0000
        .byte   0x00, 0x92, 0xCF, 0x00
        /* 0x10: code (base=0, limit=4GB, DPL=0, type=code r/x) */
        .word   0xFFFF, 0x0000
        .byte   0x00, 0x9A, 0xCF, 0x00
ap_tmp_gdt_end:

/* GDTR descriptor pointing to ap_tmp_gdt at its PHYSICAL address. */
ap_tmp_gdtdesc:
        .word   ap_tmp_gdt_end - ap_tmp_gdt - 1
        .long   TRAMPOLINE_BASE + (ap_tmp_gdt - ap_trampoline_start)

.global ap_trampoline_end
ap_trampoline_end:
