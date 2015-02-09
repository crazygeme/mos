#ifndef _PHYMM_H_
#define _PHYMM_H_

typedef struct _phymm_page
{
    unsigned long ref_count;
}phymm_page;

extern phymm_page* phymm_pages;

unsigned phymm_get_mgmt_pages(unsigned highest_mm_addr);

void phymm_setup_mgmt_pages(unsigned start_page);

unsigned phymm_reference_page(unsigned page_index);

unsigned phymm_dereference_page(unsigned page_index);

#endif
