#ifndef _PHYMM_H_
#define _PHYMM_H_

#define PHYMM_PAGE_COW 0x00000001

typedef struct _phymm_page
{
    unsigned int ref_count;
    unsigned int flags;  
}phymm_page;

extern phymm_page* phymm_pages;

unsigned phymm_get_mgmt_pages(unsigned highest_mm_addr);

void phymm_setup_mgmt_pages(unsigned start_page);

unsigned phymm_reference_page(unsigned page_index);

unsigned phymm_dereference_page(unsigned page_index);

void phymm_set_cow(unsigned page_index);

int phymm_is_cow(unsigned page_index);

void phymm_clear_cow(unsigned page_index);

#endif
