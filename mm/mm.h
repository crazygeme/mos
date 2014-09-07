#ifndef _MM_H_
#define _MM_H_

typedef struct multiboot_info multiboot_info_t;

#define KERNEL_OFFSET	0xC0000000
#define KERNEL_SIZE		0x40000000
#define PAGE_SIZE (4*1024)
#define PAGE_SIZE_MASK 0xFFFFF000
#define PG_TABLE_SIZE 1024
#define PE_TABLE_SIZE 1024
#define KERNEL_PAGE_DIR_OFFSET (KERNEL_OFFSET / (4*1024*1024))

#define OFFSET_IN_PAGE_MASK 0x00000FFF
#define OFFSET_IN_PET_MASK  0x003FF000
#define OFFSET_IN_PGT_MASK  0xFFC00000

#define ADDR_TO_PGT_OFFSET(addr)  ((addr & OFFSET_IN_PGT_MASK) >> 22)
#define ADDR_TO_PET_OFFSET(addr)  ((addr & OFFSET_IN_PET_MASK) >> 12)
#define ADDR_TO_PAGE_OFFSET(addr) (addr & OFFSET_IN_PAGE_MASK)

#define USABLE_PG_INDEX_MIN 2
#define USABLE_PG_INDEX_MAX 1023

#define PAGE_ENTRY_PRESENT  0x01  // present if set
#define PAGE_ENTRY_WRITABLE 0x02  // writable if set
#define PAGE_ENTRY_DPL_USER	0x04  // user can access if set
#define PAGE_ENTRY_WT		0x08  // write through if set
#define PAGE_ENTRY_CD		0x10  // cache disable if set

#define PAGE_ENTRY_KERNEL_CODE (PAGE_ENTRY_PRESENT)
#define PAGE_ENTRY_KERNEL_DATA (PAGE_ENTRY_PRESENT|PAGE_ENTRY_WRITABLE)
#define PAGE_ENTRY_USER_CODE (PAGE_ENTRY_PRESENT|PAGE_ENTRY_DPL_USER)
#define PAGE_ENTRY_USER_DATA (PAGE_ENTRY_PRESENT|PAGE_ENTRY_WRITABLE|PAGE_ENTRY_DPL_USER)

#define RELOAD_CR3(address) \
    __asm__ ("movl %0, %%cr3" : : "q" (address));

#define ROUND_UP(X, STEP) (((X) + (STEP) - 1) / (STEP) * (STEP))

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

int mm_add_dynamic_map(unsigned int vir, unsigned int phy, unsigned flag);

void mm_del_dynamic_map(unsigned int vir);

unsigned int  mm_get_free_phy_page_index();

void mm_set_phy_page_mask(unsigned int page_index, unsigned int used);

unsigned vm_get_usr_zone(unsigned page_count);

int do_mmap(unsigned int addr, unsigned int len,unsigned int prot,
			unsigned int flags,int fd,unsigned int offset);

int do_munmap(void *addr, unsigned length);

#endif
