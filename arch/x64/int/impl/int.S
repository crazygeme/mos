#include <config.h>
.text
.code64
.global intr_entry
intr_entry:
 cld
 /* Keep the GS/FS/ES/DS/RAX layout without locked memory XCHG.
  * Copy RAX below its original slot before DS overwrites that slot. */
 pushq %rax
 subq $32, %rsp
 movq %rax, (%rsp)
 xorl %eax, %eax
 movw %ds, %ax
 movq %rax, 32(%rsp)
 movw %es, %ax
 movq %rax, 24(%rsp)
 movw %fs, %ax
 movq %rax, 16(%rsp)
 movw %gs, %ax
 movq %rax, 8(%rsp)
 pushq %rcx
 pushq %rdx
 pushq %rbx
 pushq $0
 pushq %rbp
 pushq %rsi
 pushq %rdi
 pushq %r8
 pushq %r9
 pushq %r10
 pushq %r11
 pushq %r12
 pushq %r13
 pushq %r14
 pushq %r15
 /* Maskable IRQs cannot enter the interrupt-masked SWAPGS windows.
  * Saved CS identifies their active GS base. CPU exceptions retain the
  * MSR check: NMI/#DB can interrupt entry and #GP can fault on user exit. */
 cmpq $32, 160(%rsp)
 jb intr_paranoid_gs
 testb $3, 192(%rsp)
 jz 9f
 swapgs
 jmp 9f
intr_paranoid_gs:
 movl $0xc0000101, %ecx
 rdmsr
 testl %edx, %edx
 js 9f
 swapgs
 movq $1, 88(%rsp)
9:
 lfence /* settle the GS decision before CPU-local accesses */
 movq %rsp, %rdi
 movq %rsp, %rbx
 andq $-16, %rsp
 cmpq $2, 160(%rdi)
 je 6f
 cmpq $8, 160(%rdi)
 je 6f
 cmpq $18, 160(%rdi)
 je 6f
 cmpq $0x80, 160(%rdi)
 je 1f
 call intr_handler
 jmp 2f
1:
 call intr_syscall_handler
 jmp 2f
6:
 call arch_critical_interrupt
2:
 movq %rbx, %rsp
 jmp intr_exit
.global ret_from_fork
ret_from_fork:
 movq %rsp, %rdi
 movq %rsp, %rbx
 andq $-16, %rsp
 call do_signal
 movq %rbx, %rsp
.global ret_from_syscall
.global intr_exit
ret_from_syscall:
intr_exit:
 cli
 testb $3, 192(%rsp)
 jnz 7f
 cmpq $0, 88(%rsp)
 je 7f
 /* Nested kernel return restores the interrupted GS state. */
 swapgs
7:
 popq %r15
 popq %r14
 popq %r13
 popq %r12
 popq %r11
 popq %r10
 popq %r9
 popq %r8
 popq %rdi
 popq %rsi
 popq %rbp
 addq $8, %rsp
 popq %rbx
 popq %rdx
 popq %rcx
 popq %rax
 /* Segments are restored only for user returns. Native TLS bases remain
  * MSR-based; compatibility GS is loaded after swapping out the CPU base. */
 pushq %rax
 testb $3, 72(%rsp)
 jz 8f
 swapgs
 cmpq $USER64_CODE_SELECTOR, 72(%rsp)
 je 8f
 movw 8(%rsp), %ax
 movw %ax, %gs
 movw 16(%rsp), %ax
 movw %ax, %fs
 movw 24(%rsp), %ax
 movw %ax, %es
 movw 32(%rsp), %ax
 movw %ax, %ds
8:
 popq %rax
 addq $56, %rsp
 iretq

/* The vector list mirrors the x86 backend; #SS pushes a hardware error. */
#define zero pushq %rbp; pushq $0
#define REAL pushq (%rsp); movq %rbp, 8(%rsp)
#define STUB(NUMBER, TYPE) .text; intr##NUMBER##_stub:; TYPE; pushq $0x##NUMBER; jmp intr_entry; .data; .quad intr##NUMBER##_stub;
.data
.align 8
.global intr_stubs
intr_stubs:
STUB(00, zero) STUB(01, zero) STUB(02, zero) STUB(03, zero)
STUB(04, zero) STUB(05, zero) STUB(06, zero) STUB(07, zero)
STUB(08, REAL) STUB(09, zero) STUB(0a, REAL) STUB(0b, REAL)
STUB(0c, REAL) STUB(0d, REAL) STUB(0e, REAL) STUB(0f, zero)

STUB(10, zero) STUB(11, REAL) STUB(12, zero) STUB(13, zero)
STUB(14, zero) STUB(15, zero) STUB(16, zero) STUB(17, zero)
STUB(18, REAL) STUB(19, zero) STUB(1a, REAL) STUB(1b, REAL)
STUB(1c, zero) STUB(1d, REAL) STUB(1e, REAL) STUB(1f, zero)

STUB(20, zero) STUB(21, zero) STUB(22, zero) STUB(23, zero)
STUB(24, zero) STUB(25, zero) STUB(26, zero) STUB(27, zero)
STUB(28, zero) STUB(29, zero) STUB(2a, zero) STUB(2b, zero)
STUB(2c, zero) STUB(2d, zero) STUB(2e, zero) STUB(2f, zero)

STUB(30, zero) STUB(31, zero) STUB(32, zero) STUB(33, zero)
STUB(34, zero) STUB(35, zero) STUB(36, zero) STUB(37, zero)
STUB(38, zero) STUB(39, zero) STUB(3a, zero) STUB(3b, zero)
STUB(3c, zero) STUB(3d, zero) STUB(3e, zero) STUB(3f, zero)

STUB(40, zero) STUB(41, zero) STUB(42, zero) STUB(43, zero)
STUB(44, zero) STUB(45, zero) STUB(46, zero) STUB(47, zero)
STUB(48, zero) STUB(49, zero) STUB(4a, zero) STUB(4b, zero)
STUB(4c, zero) STUB(4d, zero) STUB(4e, zero) STUB(4f, zero)

STUB(50, zero) STUB(51, zero) STUB(52, zero) STUB(53, zero)
STUB(54, zero) STUB(55, zero) STUB(56, zero) STUB(57, zero)
STUB(58, zero) STUB(59, zero) STUB(5a, zero) STUB(5b, zero)
STUB(5c, zero) STUB(5d, zero) STUB(5e, zero) STUB(5f, zero)

STUB(60, zero) STUB(61, zero) STUB(62, zero) STUB(63, zero)
STUB(64, zero) STUB(65, zero) STUB(66, zero) STUB(67, zero)
STUB(68, zero) STUB(69, zero) STUB(6a, zero) STUB(6b, zero)
STUB(6c, zero) STUB(6d, zero) STUB(6e, zero) STUB(6f, zero)

STUB(70, zero) STUB(71, zero) STUB(72, zero) STUB(73, zero)
STUB(74, zero) STUB(75, zero) STUB(76, zero) STUB(77, zero)
STUB(78, zero) STUB(79, zero) STUB(7a, zero) STUB(7b, zero)
STUB(7c, zero) STUB(7d, zero) STUB(7e, zero) STUB(7f, zero)

STUB(80, zero) STUB(81, zero) STUB(82, zero) STUB(83, zero)
STUB(84, zero) STUB(85, zero) STUB(86, zero) STUB(87, zero)
STUB(88, zero) STUB(89, zero) STUB(8a, zero) STUB(8b, zero)
STUB(8c, zero) STUB(8d, zero) STUB(8e, zero) STUB(8f, zero)

STUB(90, zero) STUB(91, zero) STUB(92, zero) STUB(93, zero)
STUB(94, zero) STUB(95, zero) STUB(96, zero) STUB(97, zero)
STUB(98, zero) STUB(99, zero) STUB(9a, zero) STUB(9b, zero)
STUB(9c, zero) STUB(9d, zero) STUB(9e, zero) STUB(9f, zero)

STUB(a0, zero) STUB(a1, zero) STUB(a2, zero) STUB(a3, zero)
STUB(a4, zero) STUB(a5, zero) STUB(a6, zero) STUB(a7, zero)
STUB(a8, zero) STUB(a9, zero) STUB(aa, zero) STUB(ab, zero)
STUB(ac, zero) STUB(ad, zero) STUB(ae, zero) STUB(af, zero)

STUB(b0, zero) STUB(b1, zero) STUB(b2, zero) STUB(b3, zero)
STUB(b4, zero) STUB(b5, zero) STUB(b6, zero) STUB(b7, zero)
STUB(b8, zero) STUB(b9, zero) STUB(ba, zero) STUB(bb, zero)
STUB(bc, zero) STUB(bd, zero) STUB(be, zero) STUB(bf, zero)

STUB(c0, zero) STUB(c1, zero) STUB(c2, zero) STUB(c3, zero)
STUB(c4, zero) STUB(c5, zero) STUB(c6, zero) STUB(c7, zero)
STUB(c8, zero) STUB(c9, zero) STUB(ca, zero) STUB(cb, zero)
STUB(cc, zero) STUB(cd, zero) STUB(ce, zero) STUB(cf, zero)

STUB(d0, zero) STUB(d1, zero) STUB(d2, zero) STUB(d3, zero)
STUB(d4, zero) STUB(d5, zero) STUB(d6, zero) STUB(d7, zero)
STUB(d8, zero) STUB(d9, zero) STUB(da, zero) STUB(db, zero)
STUB(dc, zero) STUB(dd, zero) STUB(de, zero) STUB(df, zero)

STUB(e0, zero) STUB(e1, zero) STUB(e2, zero) STUB(e3, zero)
STUB(e4, zero) STUB(e5, zero) STUB(e6, zero) STUB(e7, zero)
STUB(e8, zero) STUB(e9, zero) STUB(ea, zero) STUB(eb, zero)
STUB(ec, zero) STUB(ed, zero) STUB(ee, zero) STUB(ef, zero)

STUB(f0, zero) STUB(f1, zero) STUB(f2, zero) STUB(f3, zero)
STUB(f4, zero) STUB(f5, zero) STUB(f6, zero) STUB(f7, zero)
STUB(f8, zero) STUB(f9, zero) STUB(fa, zero) STUB(fb, zero)
STUB(fc, zero) STUB(fd, zero) STUB(fe, zero) STUB(ff, zero)

.text
.global switch_to_user_mode
switch_to_user_mode:
 cli
 subq $224, %rsp
 movq %rsi, %rdx
 movq %rdi, %rsi
 movq %rsp, %rdi
 movq %rsp, %rbx
 andq $-16, %rsp
 call arch_task_init_user_frame
 movq %rbx, %rsp
 jmp intr_exit
.global native_syscall_entry
native_syscall_entry:
 swapgs
 movq %rsp, %gs:16
 movq %gs:8, %rsp
 pushq $USER_DATA_SELECTOR
 pushq %gs:16
 pushq %r11
 pushq $USER64_CODE_SELECTOR
 pushq %rcx
 pushq %rbp
 pushq $0
 pushq $0x80
 swapgs
 jmp intr_entry
.section .note.GNU-stack,"",@progbits
