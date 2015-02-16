#include "phymm.h"
#include "mm.h"

phymm_page* phymm_pages;

extern unsigned phymm_valid;
unsigned phymm_get_mgmt_pages(unsigned highest_mm_addr)
{
    unsigned page_count = highest_mm_addr / PAGE_SIZE;
    unsigned size = page_count * sizeof(phymm_page);
    unsigned pages = ((size-1)/PAGE_SIZE)+1;
    return pages;
}

void phymm_setup_mgmt_pages(unsigned start_page)
{
    unsigned i;
    unsigned addr;
    for (i = (start_page * PAGE_SIZE); i < (phymm_valid * PAGE_SIZE); i += PAGE_SIZE) {
        addr = i + KERNEL_OFFSET;
        mm_add_direct_map(addr);
        memset(addr, 0, PAGE_SIZE);
    }
    phymm_pages = (phymm_page*)( (start_page*PAGE_SIZE)+KERNEL_OFFSET);
}

unsigned phymm_reference_page(unsigned page_index)
{
    return __sync_add_and_fetch(&(phymm_pages[page_index].ref_count), 1);
}

unsigned phymm_dereference_page(unsigned page_index)
{
    return __sync_add_and_fetch(&(phymm_pages[page_index].ref_count), -1);
}

void phymm_set_cow(unsigned page_index)
{
    phymm_pages[page_index].flags |= PHYMM_PAGE_COW;
}

int phymm_is_cow(unsigned page_index)
{
    return ((phymm_pages[page_index].flags & PHYMM_PAGE_COW) == PHYMM_PAGE_COW);
}

void phymm_clear_cow(unsigned page_index)
{
    phymm_pages[page_index].flags &= ~PHYMM_PAGE_COW;
}
