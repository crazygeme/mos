bits 32
global bios_get_mem_size

bios_get_mem_size:
	mov ax, -1
	int	0x12
	ret
	jc	.error
	test	ax, ax		; if size=0
	je	.error
	cmp	ah, 0x86	;unsupported function
	je	.error
	cmp	ah, 0x80	;invalid command
	je	.error
	ret
.error:
	mov	ax, -1
	ret


