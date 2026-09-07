.text
/* cdecl: save the complete callee-preserved context and interrupt state.
 * The BKL remains held across this operation and is inherited by next. */
.global ps_context_switch
ps_context_switch:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushfl
    movl 24(%esp), %eax
    movl 28(%esp), %edx
    movl %esp, (%eax)
    movl %edx, %esp
    popfl
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
.section .note.GNU-stack,"",@progbits
