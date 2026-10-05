/* SysV AMD64: saved RFLAGS, r15, r14, r13, r12, rbx, rbp, return RIP. */
.text
.global ps_context_switch
.type ps_context_switch,@function
ps_context_switch:
 pushq %rbp
 pushq %rbx
 pushq %r12
 pushq %r13
 pushq %r14
 pushq %r15
 pushfq
 movq %rsp, (%rdi)
 movq %rsi, %rsp
 movq %rsp, %rbx
 movq %rdx, %rdi
 andq $-16, %rsp
 call ps_switch_finish
 movq %rbx, %rsp
 popfq
 popq %r15
 popq %r14
 popq %r13
 popq %r12
 popq %rbx
 popq %rbp
 ret
.size ps_context_switch,.-ps_context_switch
.section .note.GNU-stack,"",@progbits
