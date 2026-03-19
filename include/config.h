#ifndef _CONFIG_H
#define _CONFIG_H

#define UTS_SYSNAME "MOS"
#define UTS_NODENAME "mos"
#define UTS_RELEASE "0.1.0"
#define UTS_VERSION "#1 SMP"
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

#define SEG_BASE_4K 1 // address count with 4k
#define SEG_BASE_1 0 // address count with 1 byte
#define TSS_SEG_BASE SEG_BASE_1

#define IDT_SIZE 256

#define KHEAP_BEGIN \
	0xC0700000 // increase by 4M if kernel img is too large for this reserve

#define KHEAP_END (KHEAP_BEGIN + 0x004FF000)

#define PAGE_TABLE_CACHE_BEGIN (KHEAP_END + 0x00001000)
#define PAGE_TABLE_CACHE_END (PAGE_TABLE_CACHE_BEGIN + 0x0400000)

#define RESERVED_PAGES ((PAGE_TABLE_CACHE_END - KERNEL_OFFSET) / PAGE_SIZE)

#define DSR_CACHE_DEPTH 100

#define HDD_CACHE_SIZE 4096 /* pages */

#define HDD_CACHE_OPEN 1

#define HDD_CACHE_WRITE_THOUGH 0
#define HDD_CACHE_WRITE_BACK 1

#define HDD_CACHE_WRITE_POLICY HDD_CACHE_WRITE_BACK

#define USER_STACK_PAGES 128

#define SYSCALL_INT_NO 0x80

#define STDIN_FILENO 0 /* Standard input.  */
#define STDOUT_FILENO 1 /* Standard output.  */
#define STDERR_FILENO 2 /* Standard error output.  */

#define PIPE_BUF_LEN (4096)

#define USER_HEAP_BEGIN 0x60000000
// left one page (a hole) to protect stack overflow and heap overflow
#define USER_HEAP_END (KERNEL_OFFSET - (USER_STACK_PAGES + 1) * PAGE_SIZE)

#define USER_ZONE_BEGIN 0x10000000
#define USER_ZONE_END (USER_HEAP_BEGIN - PAGE_SIZE)

// supported resolution
#define VGA_RESOLUTION_X 768
#define VGA_RESOLUTION_Y 512
#define VGA_COLOR_DEPTH 32

#define MAX_PATH (4096)

#define PAGE_SIZE (4 * 1024)
#define PAGE_SIZE_MASK 0xFFFFF000

#define PAGE_CACHE_SIZE 4096 // pages

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
