#include "phymm.h"
#include "mm.h"
#include "time.h"
#include <ps.h>
#include <klib.h>
#include <macro.h>

unsigned phymm_used = 0;
unsigned long long phymm_alloc_spent = 0;

// if kernel == 1, then try to allocate memory from low to high
// else from high to low

static unsigned phymm_reserve_area(unsigned page_count, int kernel)
{
	int i = 0;
	unsigned first = 0;
	int match = 0;
	unsigned long long begin = time_now_us();
	if (kernel) {
		for (i = phymm_begin; i < phymm_end; i++) {
			if (phymm_pages[i].ref_count == 0) {
				match++;
			} else {
				match = 0;
				continue;
			}

			if (match == page_count) {
				first = i - match + 1;
				break;
			}
		}
	} else {
		for (i = phymm_end - 1; i >= phymm_begin; i--) {
			if (phymm_pages[i].ref_count == 0) {
				match++;
			} else {
				match = 0;
				continue;
			}

			if (match == page_count) {
				first = i;
				break;
			}
		}
	}

	phymm_alloc_spent += time_now_us() - begin;

	return first;
}

unsigned phymm_alloc_kernel(unsigned page_count)
{
	return phymm_reserve_area(page_count, 1);
}

// user memory don't has to be physical contigious,
// so if kernel == 0 we assume page_count == 1
unsigned phymm_alloc_user()
{
	return phymm_reserve_area(1, 0);
}

void phymm_free_kernel(unsigned page_index, unsigned page_count)
{
	// do nothing
}

void phymm_free_user(unsigned page_index)
{
	// do nothing
}

phymm_page *phymm_pages;
unsigned phymm_get_mgmt_pages(unsigned highest_mm_addr)
{
	unsigned page_count = highest_mm_addr / PAGE_SIZE;
	unsigned size = page_count * sizeof(phymm_page);
	unsigned pages = ((size - 1) / PAGE_SIZE) + 1;
	return pages;
}

void phymm_setup_mgmt_pages(unsigned start_page)
{
	unsigned i;
	unsigned addr;
	for (i = (start_page * PAGE_SIZE); i < (phymm_begin * PAGE_SIZE);
	     i += PAGE_SIZE) {
		addr = i + KERNEL_OFFSET;
		mm_add_direct_map(addr);
	}
	RELOAD_CR3();
	addr = (start_page * PAGE_SIZE) + KERNEL_OFFSET;
	memset((void *)addr, 0, (phymm_begin - start_page) * PAGE_SIZE);
	phymm_pages = (phymm_page *)(addr);
}

unsigned phymm_reference_page(unsigned page_index)
{
	unsigned ret =
		__sync_add_and_fetch(&(phymm_pages[page_index].ref_count), 1);
	if (ret == 1) {
		phymm_used++;
	}
}

unsigned phymm_dereference_page(unsigned page_index)
{
	unsigned ret =
		__sync_add_and_fetch(&(phymm_pages[page_index].ref_count), -1);
	if (ret == 0) {
		phymm_used--;
	}
}

int phymm_is_cow(unsigned page_index)
{
	int ret = __sync_add_and_fetch(&(phymm_pages[page_index].ref_count), 0);
	return (ret > 1);
}

int phymm_is_used(unsigned page_index)
{
	int ret = __sync_add_and_fetch(&(phymm_pages[page_index].ref_count), 0);
	return (ret > 0);
}
