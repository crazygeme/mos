//#include "config.h"
#define MBALIGN (1<<0)					// align loaded modules on page boundaries
#define MEMINFO (1<<1)					// provide memory map
#define FLAGS (MBALIGN | MEMINFO)		// this is the Multiboot 'flag' field
#define MAGIC 0x1BADB002				//'magic number' lets bootloader find the header
#define CHECKSUM (-(MAGIC + FLAGS))        // checksum of above, to prove we are multiboot
.align 4
.section .multiboot 
	var:	.long MAGIC
			.long FLAGS
			.long CHECKSUM
			.long 0

.align 4
.section .bootstrap_stack
stack_bottom: .zero 16384
stack_top: .byte 0

.section .start

.global __start
.func __start
__start:
	movl $stack_top, %ecx
	subl $0XC0000000, %ecx 
	movl %ecx, %esp
	pushl %eax
	pushl %ebx 
	call kmain // never returns
	cli
hang:
	hlt
	jmp hang
.endfunc

.section .start
.global write_port
.global read_port

.func write_port
write_port:
  movl 4(%esp), %edx
  mov  8(%esp), %al
  outb   %al, %dx 
  ret
.endfunc

.func read_port
read_port:
  movl  4(%esp), %edx 
  xor   %eax, %eax  
  inb   %dx, %al 
  ret 
.endfunc

.section .text
.global _write_port
.global _read_port

.func _read_port
_read_port:
  movl  4(%esp), %edx 
  xor   %eax, %eax  
  inb   %dx, %al 
  ret 
.endfunc

.func _write_port
_write_port:
  movl 4(%esp), %edx
  mov  8(%esp), %al
  outb   %al, %dx 
  ret
.endfunc

.global vm_page_inval
.func vm_page_inval
vm_page_inval:
	mov %cr3, %eax
	mov %eax, %cr3
	ret
.endfunc

