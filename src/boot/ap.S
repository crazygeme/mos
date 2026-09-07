#include <config.h>

/* Copied to physical 0x7000; no link-address references in real mode. */
.section .rodata
.balign 16
.global smp_trampoline, smp_trampoline_end
smp_trampoline:
.code16
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    lgdt 0x7000 + (ap_gdtr - smp_trampoline)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $KERNEL_CODE_SELECTOR, $0x7000 + (ap_protected - smp_trampoline)
.code32
ap_protected:
    movw $KERNEL_DATA_SELECTOR, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl 0x7ff0, %eax
    movl %eax, %cr3
    movl %cr0, %eax
    orl $0x80010000, %eax
    movl %eax, %cr0
    movl 0x7ff4, %esp
    xorl %ebp, %ebp
    movl 0x7ff8, %eax
    jmp *%eax
.balign 8
ap_gdt:
    .quad 0
    .quad 0x00cf92000000ffff
    .quad 0x00cf9a000000ffff
ap_gdtr:
    .word 23
    .long 0x7000 + (ap_gdt - smp_trampoline)
smp_trampoline_end:
.code32
.section .note.GNU-stack,"",@progbits
