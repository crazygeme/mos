.text
/* cdecl: save the complete callee-preserved context and interrupt state.
 * The scheduler lock is released on the incoming stack. */
.global ps_context_switch
ps_context_switch:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushfl
    movl 24(%esp), %eax
    movl 28(%esp), %edx
    movl 32(%esp), %ecx
    movl %esp, (%eax)
    movl %edx, %esp
    pushl %ecx
    call ps_switch_finish
    addl $4, %esp
    popfl
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
.section .note.GNU-stack,"",@progbits
