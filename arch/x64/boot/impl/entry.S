#include <config.h>
.section .multiboot,"a"
.align 4
.long 0x1badb002, 0x10003, -(0x1badb002 + 0x10003)
/* The address fields allow a flat Multiboot loader to enter code32. */
.long 0x100000, 0x100000, __boot_load_end, __boot_bss_end, __start

.section .boot.data,"aw"
.align 8
boot_gdt:
.quad 0
.quad 0x00af9a000000ffff /* ring 0, L=1 D=0 */
.quad 0x00cf92000000ffff
.quad 0x00cff2000000ffff
.quad 0x00cffa000000ffff /* ring 3, L=0 D=1: i386 compatibility */
.quad 0x00affa000000ffff /* ring 3, L=1 D=0: AMD64 */
boot_gdt_end:
boot_gdtr:
.word boot_gdt_end - boot_gdt - 1
.long boot_gdt

.section .boot.bss,"aw",@nobits
.align 4096
.global boot_pml4
boot_pml4: .skip 4096
boot_low_pdpt: .skip 4096
boot_high_pdpt: .skip 4096
boot_pd: .skip 4096
.align 16384
boot_stack: .skip 16384
boot_stack_top:

.section .boot.text,"ax"
.code32
.global __start
.type __start,@function
__start:
 cli
 cld
 movl $boot_stack_top, %esp
 movl %eax, %esi
 movl %ebx, %edi
 cmpl $0x2badb002, %esi
 jne boot_halt
 movl $0x80000000, %eax
 cpuid
 cmpl $0x80000001, %eax
 jb boot_halt
 movl $0x80000001, %eax
 cpuid
 testl $(1 << 29), %edx /* long mode */
 jz boot_halt
 testl $(1 << 20), %edx /* NX */
 jz boot_halt
 /* Bootstrap tables are physical; both aliases are supervisor-only. */
 movl $boot_pml4, %eax
 movl $(4 * 4096 / 4), %ecx
 pushl %edi
 movl %eax, %edi
 xorl %eax, %eax
 rep stosl
 popl %edi
 movl $(boot_low_pdpt + 3), boot_pml4
 movl $(boot_high_pdpt + 3), boot_pml4 + 511 * 8
 movl $(boot_pd + 3), boot_low_pdpt
 movl $(boot_pd + 3), boot_high_pdpt + 511 * 8
 xorl %ecx, %ecx
 movl $0x83, %eax
1:
 movl %eax, boot_pd(,%ecx,8)
 addl $0x200000, %eax
 incl %ecx
 cmpl $512, %ecx
 jne 1b
 lgdt boot_gdtr
 movl %cr4, %eax
 orl $(1 << 5), %eax /* PAE */
 movl %eax, %cr4
 movl $boot_pml4, %eax
 movl %eax, %cr3
 movl $0xc0000080, %ecx
 rdmsr
 orl $((1 << 8) | (1 << 11)), %eax /* EFER.LME */
 wrmsr
 movl %cr0, %eax
 orl $0x80010001, %eax /* PG, WP, PE */
 movl %eax, %cr0
 ljmp $KERNEL_CODE_SELECTOR, $boot_long_mode
boot_halt:
 cli
 hlt
 jmp boot_halt
.code64
boot_long_mode:
 movw $KERNEL_DATA_SELECTOR, %ax
 movw %ax, %ds
 movw %ax, %es
 movw %ax, %ss
 xorw %ax, %ax
 movw %ax, %fs
 movw %ax, %gs
 movabsq $MOS_KERNEL_OFFSET, %rax
 addq %rax, %rsp
 subq $8, %rsp /* SysV C entry alignment for the non-returning jump */
 /* EBX information survives CPUID in EDI; ESI retains the boot magic. */
 movabsq $boot_stage1, %rax
 jmp *%rax
.section .note.GNU-stack,"",@progbits
