#include "phymm.h"
#include "mm.h"
#include <ps.h>
#define SIZE_TO_FREE_LIST_INDEX(page_count) (\
    (page_count <= 1)   ? 0 : \
    (page_count <= 2)   ? 1 : \
    (page_count <= 4)   ? 2 : \
    (page_count <= 8)   ? 3 : \
    (page_count <= 16)  ? 4 : \
    (page_count <= 32)  ? 5 : \
    (page_count <= 64)  ? 6 : \
    (page_count <= 128) ? 7 : \
    (page_count <= 256) ? 8 : \
    (page_count <= 512) ? 9 : \
    (page_count <= (1    * 1024)) ? 10 : \
    (page_count <= (2    * 1024)) ? 11 : \
    (page_count <= (4    * 1024)) ? 12 : \
    (page_count <= (8    * 1024)) ? 13 : \
    (page_count <= (16   * 1024)) ? 14 : \
    (page_count <= (32   * 1024)) ? 15 : \
    (page_count <= (64   * 1024)) ? 16 : \
    (page_count <= (128  * 1024)) ? 17 : \
    (page_count <= (256  * 1024)) ? 18 : \
    (page_count <= (512  * 1024)) ? 19 : \
    (page_count <= (1024 * 1024)) ? 20 : -1)

extern unsigned phymm_max;
extern unsigned phymm_valid;

typedef struct  free_page_lists
{
    unsigned int head[21];
};

static struct free_page_lists kernel_free_list = {0};
static unsigned user_head = 0;
// if kernel == 1, then try to allocate memory from low to high
// else from high to low

#define ALLOCATE_PHYMM(list, page_count, kernel, ret)\
    do {\
        if (list == 0) { \
            phymm_reserve_area(&list, page_count, kernel); \
        }\
        ret = list;\
        list = phymm_pages[ret].next_page_index;\
        phymm_pages[ret].next_page_index = 0; \
    }while(0)

#define FREE_PHYMM(list, page_index)\
    do {\
        phymm_pages[page_index].next_page_index = list;\
        list = page_index;\
    }while(0)



static void phymm_reserve_area(unsigned* list, unsigned page_count, int kernel)
{
    int i = 0;
    unsigned first = 0;
    int match = 0;
    if (kernel)
    {

        for (i = phymm_valid; i < phymm_max; i++)
        {
            if (!phymm_pages[i].reserved)
            {
                match++;
            }
            else
            {
                match = 0;
                continue;
            }

            if (match == page_count)
            {
                first = i-match+1;
                break;
            }
        }
    }
    else
    {
        for (i = phymm_max-1; i >= phymm_valid; i--)
        {
            if (!phymm_pages[i].reserved)
            {
                match++;
            }
            else
            {
                match = 0;
                continue;
            }

            if (match == page_count)
            {
                first = i;
                break;
            }
        }
    }

    // no more memory, just die
    if (!first)
        return;

    for (i = first; i < (first+page_count); i++)
    {
        phymm_pages[i].reserved = 1;
    }

    phymm_pages[first].next_page_index = *list;
    *list = first;
}

unsigned phymm_alloc_kernel(unsigned page_count)
{
    int index = SIZE_TO_FREE_LIST_INDEX(page_count);
    int first = 0;
    if (index == -1){
        return -1;
    }

    ALLOCATE_PHYMM(kernel_free_list.head[index], page_count, 1, first);

    return first;
        
}

// user memory don't has to be physical contigious, 
// so if kernel == 0 we assume page_count == 1
unsigned phymm_alloc_user()
{
    int first = 0;

    ALLOCATE_PHYMM(user_head, 1, 0, first);

    return first;
}

void phymm_free_kernel(unsigned page_index, unsigned page_count)
{
    int index = SIZE_TO_FREE_LIST_INDEX(page_count);
    int first = 0;
    if (index == -1){
        return -1;
    }

    FREE_PHYMM(kernel_free_list.head[index], page_index);
}

void phymm_free_user(unsigned page_index)
{
    FREE_PHYMM(user_head, page_index);
}


phymm_page* phymm_pages;
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
    for (i = (start_page * PAGE_SIZE); i < (phymm_valid * PAGE_SIZE); i += PAGE_SIZE)
    {
        addr = i + KERNEL_OFFSET;
        mm_add_direct_map(addr);
        memset(addr, 0, PAGE_SIZE);
    }
    phymm_pages = (phymm_page*)((start_page*PAGE_SIZE) + KERNEL_OFFSET);
}

unsigned phymm_reference_page(unsigned page_index)
{
    return __sync_add_and_fetch(&(phymm_pages[page_index].ref_count), 1);
}

unsigned phymm_dereference_page(unsigned page_index)
{
    return __sync_add_and_fetch(&(phymm_pages[page_index].ref_count), -1);
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

