/* IA-32 bootstrap followed by the AMD64 high-half kernel. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
SECTIONS
{
	ENTRY(__start)

	. = 0x100000;

	.multiboot BLOCK(64K) : AT(ADDR(.multiboot))
	{
		KEEP(*(.multiboot))
		*(.boot.text)
		*(.boot.data)
	}

	.boot.bss ALIGN(4096) (NOLOAD) : { *(.boot.bss) }
	ASSERT(. < 0x200000, "bootstrap overlaps kernel")

	/* Begin putting sections at 1 MiB, a conventional place for kernels to be
	   loaded at by the bootloader. */
	. = 0xffffffffc0000000 + 0x200000;

	/* First put the multiboot header, as it is required to be put very early
	   early in the image or the bootloader won't recognize the file format.
	   Next we'll put the .text section. */
	.text BLOCK(64K) :  AT(ADDR(.text)-0xffffffffc0000000)
	{
		*(.text .text.*)
	}

	/* Read-only data. */
	.rodata BLOCK(4K) : AT(ADDR(.rodata)-0xffffffffc0000000)
	{
		*(.rodata .rodata.*)
	}

	.procfs_init BLOCK(4K) : AT(ADDR(.procfs_init)-0xffffffffc0000000)
	{
		__procfs_init_start = .;
		*(.procfs_init)
		__procfs_init_end = .;
	}

	.devfs_init BLOCK(4K) : AT(ADDR(.devfs_init)-0xffffffffc0000000)
	{
		__devfs_init_start = .;
		*(.devfs_init)
		__devfs_init_end = .;
	}

	/*
	 * Kernel test table.  Each KTEST(name, fn) declaration drops a
	 * ktest_t into ".ktest".  The /proc/test driver iterates this
	 * section to discover and run all registered tests.
	 */
	.ktest BLOCK(4K) : AT(ADDR(.ktest)-0xffffffffc0000000)
	{
		__ktest_start = .;
		*(.ktest)
		__ktest_end = .;
	}

	.ktest_script BLOCK(4K) : AT(ADDR(.ktest_script)-0xffffffffc0000000)
	{
		__ktest_script_start = .;
		*(.ktest_script)
		__ktest_script_end = .;
	}

	/*
	 * Kernel init-call table.  Each KERNEL_INIT(index, fn) declaration
	 * drops a kinit_fn_t into ".kinit.<index>".  SORT_BY_NAME guarantees
	 * the sections are laid out in ascending index order.
	 */
	.kinit BLOCK(4K) : AT(ADDR(.kinit)-0xffffffffc0000000)
	{
		__kinit_start = .;
		*(SORT_BY_NAME(.kinit.*))
		__kinit_end = .;
	}

	/* Drivers are registered before physical device discovery. */
	.driver BLOCK(4K) : AT(ADDR(.driver)-0xffffffffc0000000)
	{
		__driver_start = .;
		KEEP(*(.driver))
		__driver_end = .;
	}

	.vdso BLOCK(4K): AT(ADDR(.vdso)-0xffffffffc0000000)
	{
		__vdso_start = .;
		*(.vdso)
		__vdso_end = .;
	}

	/* Read-write data (initialized) */
	.data BLOCK(4K) : AT(ADDR(.data)-0xffffffffc0000000)
	{
		*(.data .data.*)
	}

 __boot_load_end = LOADADDR(.data) + SIZEOF(.data);
	/* Read-write data (uninitialized) and stack */
	.bss BLOCK(4K) : AT(ADDR(.bss)-0xffffffffc0000000)
	{
		*(COMMON)
		*(.bss .bss.*)
		*(.bootstrap_stack)
		__kernel_end = .;
	}
	
	ASSERT(__kernel_end <= 0xffffffffc0700000, "kernel overlaps static heap")
 __boot_bss_end = __kernel_end - 0xffffffffc0000000;
 /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
	/* The compiler may produce other sections, by default it will put them in
	   a segment with the same name. Simply add stuff here as needed. */
}
