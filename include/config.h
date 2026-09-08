#ifndef _CONFIG_H
#define _CONFIG_H

#include <arch/config.h>

#define UTS_SYSNAME "Linux"
#define UTS_NODENAME "mos"
#define UTS_RELEASE "2.4.20-8"
#define UTS_VERSION "#1 Thu Mar 13 17:54:28 EST 2003"
#define UTS_MACHINE "i686"

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

#define HDD_CACHE_OPEN 1

#define HDD_CACHE_WRITE_THOUGH 0
#define HDD_CACHE_WRITE_BACK 1

#define HDD_CACHE_WRITE_POLICY HDD_CACHE_WRITE_BACK

/* Block-cache pages hold persistent kmap aliases.  Bound them well below the
 * kmap window size so fault-time temporary mappings cannot be starved. */
#define HDD_CACHE_MAX_PAGES 16384 /* 64 MiB */

#define USER_STACK_PAGES 4096
/* Initial stack pages allocated at exec; stack grows down to USER_STACK_PAGES max. */
#define USER_STACK_INIT_PAGES 16

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

#endif
