#ifndef _PHYMM_H_
#define _PHYMM_H_
#include <config.h>
#define PHYMM_PAGE_COW 0x00000001

typedef struct _phymm_page
{
    unsigned int ref_count;
    unsigned int next_page_index : 20;
    unsigned int reserved : 12;
}phymm_page;

extern phymm_page* phymm_pages;

unsigned phymm_get_mgmt_pages(unsigned highest_mm_addr);

void phymm_setup_mgmt_pages(unsigned start_page);

unsigned phymm_reference_page(unsigned page_index);

unsigned phymm_dereference_page(unsigned page_index);

int phymm_is_cow(unsigned page_index);

unsigned phymm_alloc_kernel(unsigned page_count);

unsigned phymm_alloc_user();

void phymm_free_kernel(unsigned page_index, unsigned page_count);

void phymm_free_user(unsigned page_index);

int phymm_is_used(unsigned page_index);

#endif
