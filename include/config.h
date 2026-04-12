#ifndef _CONFIG_H
#define _CONFIG_H

#define UTS_SYSNAME "Linux"
#define UTS_NODENAME "mos"
#define UTS_RELEASE "2.4.20-8"
#define UTS_VERSION "#1 Thu Mar 13 17:54:28 EST 2003"
#define UTS_MACHINE "i686"

#define KERNEL_PRIVILEGE 0
#define USER_PRIVILEGE 3

#define NULL_SELECTOR 0x0
#define KERNEL_DATA_SELECTOR 0x8
#define KERNEL_CODE_SELECTOR 0x10
#define USER_DATA_SELECTOR 0x1b
#define USER_CODE_SELECTOR 0x23
/* TSS_SELECTOR: not used for task switching, but TR must be valid on x86 */
#define TSS_SELECTOR 0x28
/* TSS selectors: CPU 0 at TSS_SELECTOR, CPU n at TSS_SELECTOR + n*8 */
/* SELECTOR_COUNT is now defined after MAX_CPUS below */

#define ADDRESS_LIMIT \
	0xfffff //  always 4k bytes algined, so last 0xfffff means 4G space

#define SEG_CLASS_DATA 1
#define SEG_CLASS_SYSTEM 0 // this is for TSS

/* GDT entries reserved for per-process TLS (set_thread_area / Linux compat) */
#define GDT_ENTRY_TLS_MIN 6
#define GDT_ENTRY_TLS_MAX 8
#define GDT_ENTRY_TLS_COUNT 3

#define SEG_BASE_4K 1 // address count with 4k
#define SEG_BASE_1 0 // address count with 1 byte
#define TSS_SEG_BASE SEG_BASE_1

#define IDT_SIZE 256

#define KHEAP_BEGIN \
	0xC0700000 // increase by 4M if kernel img is too large for this reserve

#define KHEAP_END (KHEAP_BEGIN + 0x004FF000)

#define PAGE_TABLE_CACHE_PAGES 4096 /* number of 4K page tables in the cache */
#define PAGE_TABLE_CACHE_BEGIN (KHEAP_END + 0x00001000)
#define PAGE_TABLE_CACHE_END \
	(PAGE_TABLE_CACHE_BEGIN + PAGE_TABLE_CACHE_PAGES * PAGE_SIZE)

#define RESERVED_PAGES ((PAGE_TABLE_CACHE_END - KERNEL_OFFSET) / PAGE_SIZE)

#define DSR_CACHE_DEPTH 4096

#define HDD_CACHE_SIZE (4096) /* pages */

#define HDD_CACHE_OPEN 1

#define HDD_CACHE_WRITE_THOUGH 0
#define HDD_CACHE_WRITE_BACK 1

#define HDD_CACHE_WRITE_POLICY HDD_CACHE_WRITE_BACK

#define USER_STACK_PAGES 4096
/* Initial stack pages allocated at exec; stack grows down to USER_STACK_PAGES max. */
#define USER_STACK_INIT_PAGES 16

#define SYSCALL_INT_NO 0x80

#define STDIN_FILENO 0 /* Standard input.  */
#define STDOUT_FILENO 1 /* Standard output.  */
#define STDERR_FILENO 2 /* Standard error output.  */

#define TASK_UNMAPPED_BASE \
	(KERNEL_OFFSET / 3) /* 0x40000000, matches Linux i386 */
/* heap is bounded by the mmap zone, matching Linux classic VM layout */
#define USER_HEAP_END TASK_UNMAPPED_BASE
/* mmap zone: [TASK_UNMAPPED_BASE, USER_ZONE_END), top is the max stack floor */
#define USER_ZONE_END (KERNEL_OFFSET - USER_STACK_PAGES * PAGE_SIZE)

// supported resolution
#define VGA_RESOLUTION_X 800
#define VGA_RESOLUTION_Y 600
#define VGA_COLOR_DEPTH 32

#define MAX_PATH (4096 - sizeof(list_entry))

#define PAGE_SIZE (4 * 1024)
#define PAGE_SIZE_MASK 0xFFFFF000

#define PAGE_CACHE_SIZE (4096 * 16) // pages
#define PAGE_PREFETCH_N \
	16 /* read-ahead: (N-1) pages before, N pages after fault */

/* SMP configuration */
#define MAX_CPUS 8
#define AP_TRAMPOLINE_PHYS 0x8000 /* physical addr for AP startup code */
#define AP_PARAMS_PHYS 0x9000 /* physical addr for AP params page */
#define IPI_VECTOR_SCHED 0xF0 /* scheduler kick IPI */
#define IPI_VECTOR_TLB 0xF1 /* TLB shootdown IPI */
#define IPI_VECTOR_SPURIOUS 0xFF /* spurious APIC interrupt */

/* GDT: 5 base entries + one TSS per CPU */
#define SELECTOR_COUNT (5 + MAX_CPUS)

#endif
