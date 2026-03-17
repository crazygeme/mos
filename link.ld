SECTIONS
{
	ENTRY(__start)

	. = 0x100000;

	.multiboot BLOCK(64K) : AT(ADDR(.multiboot))
	{
		*(.multiboot)
	}

	/* Begin putting sections at 1 MiB, a conventional place for kernels to be
	   loaded at by the bootloader. */
	. = 0xC0000000 + 0x200000;

	/* First put the multiboot header, as it is required to be put very early
	   early in the image or the bootloader won't recognize the file format.
	   Next we'll put the .text section. */
	.text BLOCK(64K) :  AT(ADDR(.text)-0xC0000000)
	{
		*(.text)
	}

	.user BLOCK(4K): AT(ADDR(.user)-0xC0000000)
	{
		*(.user)
	}

	.userdata BLOCK(4K): AT(ADDR(.userdata)-0xC0000000)
	{
		*(.userdata)
	}

	/* Read-only data. */
	.rodata BLOCK(4K) : AT(ADDR(.rodata)-0xC0000000)
	{
		*(.rodata)
	}

	.procfs_init BLOCK(4K) : AT(ADDR(.procfs_init)-0xC0000000)
	{
		__procfs_init_start = .;
		*(.procfs_init)
		__procfs_init_end = .;
	}

	/*
	 * Kernel init-call table.  Each KERNEL_INIT(index, fn) declaration
	 * drops a kinit_fn_t into ".kinit.<index>".  SORT_BY_NAME guarantees
	 * the sections are laid out in ascending index order.
	 */
	.kinit BLOCK(4K) : AT(ADDR(.kinit)-0xC0000000)
	{
		__kinit_start = .;
		*(SORT_BY_NAME(.kinit.*))
		__kinit_end = .;
	}

	/* Read-write data (initialized) */
	.data BLOCK(4K) : AT(ADDR(.data)-0xC0000000)
	{
		*(.data)
	}

	/* Read-write data (uninitialized) and stack */
	.bss BLOCK(4K) : AT(ADDR(.bss)-0xC0000000)
	{
		*(COMMON)
		*(.bss)
		*(.bootstrap_stack)
	}
	
	.comment BLOCK(4K) : AT(ADDR(.comment)-0xC0000000)
	{
		*(.comment)
	}
	/* The compiler may produce other sections, by default it will put them in
	   a segment with the same name. Simply add stuff here as needed. */
}
