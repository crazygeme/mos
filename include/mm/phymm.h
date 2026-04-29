#ifndef _MM_PHYMM_H
#define _MM_PHYMM_H
#include <config.h>

#define PHYMM_PAGE_COW 0x00000001 /* kept for legacy compat (unused) */
#define PHYMM_PAGE_DIRTY 0x01 /* page written via MAP_SHARED mapping */
/* Sentinel values for ref_count and order fields */
#define PHYMM_INVALID 0xFFFFFFFFu /* end-of-list / invalid page index */
#define PHYMM_RESERVED 0xFFFFFFFEu /* ref_count: page is non-RAM / firmware */

/* Values stored in phymm_page.order */
#define MAX_BUDDY_ORDER 10 /* largest block = 2^10 = 1024 pages (4 MB) */
#define PHYMM_ORDER_NONE 0xFE /* page is part of a larger block (not head) */

typedef struct _phymm_page {
	unsigned int
		ref_count; /* 0=free, PHYMM_RESERVED=non-RAM, >0=in use      */
	unsigned int
		next_free; /* buddy free list: next page idx, PHYMM_INVALID   */
	unsigned int
		prev_free; /* buddy free list: prev page idx, PHYMM_INVALID   */
	unsigned char order; /* buddy order of this block (head page only)      */
	unsigned char flags; /* PHYMM_PAGE_* flags                              */
	unsigned char _pad[2];
} phymm_page;

typedef struct _phymm_usage {
	unsigned low_total_pages;
	unsigned low_used_pages;
	unsigned low_free_pages;
	unsigned high_total_pages;
	unsigned high_used_pages;
	unsigned high_free_pages;
} phymm_usage;

extern phymm_page *phymm_pages;

/* Called once at boot after phymm_setup_mgmt_pages.
 * Scans the multiboot mmap, marks non-RAM holes as reserved, and
 * populates the buddy free lists with all available physical pages. */
void phymm_init(unsigned mmap_addr, unsigned mmap_len);

unsigned phymm_get_mgmt_pages(unsigned highest_mm_addr);

void phymm_setup_mgmt_pages(unsigned start_page);

unsigned phymm_reference_page(unsigned page_index);

unsigned phymm_dereference_page(unsigned page_index);

int phymm_is_cow(unsigned page_index);

/* Allocate 2^ceil(log2(page_count)) contiguous physical pages.
 * Returns the starting page index, or PHYMM_INVALID on failure. */
unsigned phymm_alloc_kernel(unsigned page_count);

/* Allocate a single physical page for user space.
 * Returns the page index, or PHYMM_INVALID on failure. */
unsigned phymm_alloc_user(void);

/* Return a kernel block (starting at page_index) to the allocator. */
void phymm_free_kernel(unsigned page_index, unsigned page_count);

/* Return a single user page to the allocator. */
void phymm_free_user(unsigned page_index);

int phymm_is_used(unsigned page_index);

void phymm_get_usage(phymm_usage *usage);

/* Dirty-page tracking for MAP_SHARED file-backed mappings. */
void phymm_mark_dirty(unsigned page_index);
int phymm_is_dirty(unsigned page_index);
void phymm_clear_dirty(unsigned page_index);

#endif
