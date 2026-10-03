#include <config.h>
/* Copied to 0x7000. Parameter qwords are at 0x7fc0..0x7fdf. */
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
 ljmpl $0x18, $0x7000 + (ap_protected - smp_trampoline)
.code32
ap_protected:
 movw $KERNEL_DATA_SELECTOR, %ax
 movw %ax, %ds
 movw %ax, %es
 movw %ax, %ss
 movl 0x7fd8, %eax
 orl $(1 << 5), %eax
 movl %eax, %cr4
 movl 0x7fc0, %eax
 movl %eax, %cr3
 movl $0xc0000080, %ecx
 rdmsr
 orl $((1 << 8) | (1 << 11)), %eax
 wrmsr
 movl %cr0, %eax
 orl $0x80010000, %eax
 movl %eax, %cr0
 ljmpl $KERNEL_CODE_SELECTOR, $0x7000 + (ap_long - smp_trampoline)
.code64
ap_long:
 movq 0x7fc8, %rsp
 movq 0x7fd0, %rax
 xorq %rbp, %rbp
 jmp *%rax
.balign 8
ap_gdt:
 .quad 0
 .quad 0x00af9a000000ffff
 .quad 0x00cf92000000ffff
 .quad 0x00cf9a000000ffff
ap_gdtr:
 .word 31
 .long 0x7000 + (ap_gdt - smp_trampoline)
smp_trampoline_end:
.code64
.section .note.GNU-stack,"",@progbits
