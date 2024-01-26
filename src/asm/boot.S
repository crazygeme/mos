#include "config.h"
#define MBALIGN (1<<0)                  // align loaded modules on page boundaries
#define MEMINFO (1<<1)                  // provide memory map
#define FLAGS (MBALIGN | MEMINFO)       // this is the Multiboot 'flag' field
#define MAGIC 0x1BADB002                //'magic number' lets bootloader find the header
#define CHECKSUM (-(MAGIC + FLAGS))     // checksum of above, to prove we are multiboot


.section              .multiboot

.align                 4
var:                  .long MAGIC
                      .long FLAGS
                      .long CHECKSUM
                      .long 0

.align                4
.section              .bss
stack_bottom:         .zero 16384
stack_top:            .byte 0


.section              .multiboot

.func __start
__start:
    movl $stack_top, %ecx
    subl $0XC0000000, %ecx 
    movl %ecx, %esp
    pushl %eax
    pushl %ebx 
    call boot_stage1 // never returns
    cli
hang:
    hlt
    jmp hang
.endfunc

.global __start
