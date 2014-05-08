#ifndef _MM_H_
#define _MM_H_

typedef struct multiboot_info multiboot_info_t;

#define KERNEL_OFFSET	0xC0000000
#define KERNEL_SIZE		0x40000000
#define PAGE_SIZE (4*1024)
#define PAGE_SIZE_MASK 0xFFFFF000
#define PG_TABLE_SIZE 1024
#define PE_TABLE_SIZE 1024

#define OFFSET_IN_PAGE_MASK 0x00000FFF
#define OFFSET_IN_PET_MASK  0x003FF000
#define OFFSET_IN_PGT_MASK  0xFFC00000

#define ADDR_TO_PGT_OFFSET(addr)  ((addr & OFFSET_IN_PGT_MASK) >> 22)
#define ADDR_TO_PET_OFFSET(addr)  ((addr & OFFSET_IN_PET_MASK) >> 12)
#define ADDR_TO_PAGE_OFFSET(addr) (addr & OFFSET_IN_PAGE_MASK)

#define USABLE_PG_INDEX_MIN 2
#define USABLE_PG_INDEX_MAX 1023

#define _START \
    __attribute__((section(".start")))

#define _STARTDATA \
    __attribute__((section(".startdata")))

#define _STR \
    __attribute__((section(".startdataro")))

_START void mm_init(multiboot_info_t* mb);

#ifdef TEST_MM
void mm_test();
#endif

// map 0xCxxxxxxxx to xxxxxxx
// return (is used for page table)
int mm_add_direct_map(unsigned int vir);

void mm_del_direct_map(unsigned int vir);

void mm_del_user_map();

extern void vm_page_inval();

unsigned int mm_alloc_page_table();

void mm_free_page_table(unsigned int vir);

unsigned int vm_alloc(int page_count);

void vm_free(unsigned int vm, int page_count);

#endif
