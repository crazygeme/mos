#ifndef _MM_MM_H
#define _MM_MM_H
#include <config.h>
#include <arch/types.h>
#include <stddef.h>

typedef struct _file file;
typedef struct multiboot_info multiboot_info_t;

#define KERNEL_OFFSET MOS_KERNEL_OFFSET
#define KERNEL_SIZE MOS_KERNEL_SIZE
#define KERNEL_KMAP_BEGIN MOS_KERNEL_KMAP_BEGIN
#define KERNEL_KMAP_END MOS_KERNEL_KMAP_END
#define KERNEL_IO_BEGIN MOS_KERNEL_IO_BEGIN
#define KERNEL_IO_END MOS_KERNEL_IO_END
#define KERNEL_DIRECT_MAP_LIMIT (KERNEL_KMAP_BEGIN - KERNEL_OFFSET)
#define PG_TABLE_SIZE MOS_PG_TABLE_SIZE
#define PE_TABLE_SIZE MOS_PE_TABLE_SIZE
#define KERNEL_PAGE_DIR_OFFSET MOS_KERNEL_PAGE_DIR_OFFSET

#define OFFSET_IN_PAGE_MASK (MOS_PAGE_SIZE - 1)
#define OFFSET_IN_PET_MASK MOS_OFFSET_IN_PET_MASK
#define OFFSET_IN_PGT_MASK MOS_OFFSET_IN_PGT_MASK

#define ADDR_TO_PGT_OFFSET(addr) ((addr & OFFSET_IN_PGT_MASK) >> MOS_PGT_SHIFT)
#define ADDR_TO_PET_OFFSET(addr) ((addr & OFFSET_IN_PET_MASK) >> MOS_PET_SHIFT)
#define ADDR_TO_PAGE_OFFSET(addr) (addr & OFFSET_IN_PAGE_MASK)

#define PAGE_ENTRY_PRESENT 0x01 // present if set
#define PAGE_ENTRY_WRITABLE 0x02 // writable if set
#define PAGE_ENTRY_DPL_USER 0x04 // user can access if set
#define PAGE_ENTRY_WT 0x08 // write through if set
#define PAGE_ENTRY_CD 0x10 // cache disable if set
#define PAGE_ENTRY_ACCESSED 0x20 // CPU has accessed this page
#define PAGE_ENTRY_DIRTY 0x40 // CPU has written this page

#define PAGE_ENTRY_KERNEL_CODE (PAGE_ENTRY_PRESENT)
#define PAGE_ENTRY_KERNEL_DATA (PAGE_ENTRY_PRESENT | PAGE_ENTRY_WRITABLE)
#define PAGE_ENTRY_USER_CODE (PAGE_ENTRY_PRESENT | PAGE_ENTRY_DPL_USER)
#define PAGE_ENTRY_USER_DATA \
	(PAGE_ENTRY_PRESENT | PAGE_ENTRY_WRITABLE | PAGE_ENTRY_DPL_USER)

#define GDT_ADDRESS 0x1C0000

#define PROT_READ 0x1 /* Page can be read.  */
#define PROT_WRITE 0x2 /* Page can be written.  */
#define PROT_EXEC 0x4 /* Page can be executed.  */
#define PROT_NONE 0x0 /* Page can not be accessed.  */

#define MAP_SHARED 0x01 /* Share changes */
#define MAP_PRIVATE 0x02 /* Changes are private */
#define MAP_TYPE 0x0f /* Mask for type of mapping */
#define MAP_FIXED 0x10 /* Interpret addr exactly */
#define MAP_ANONYMOUS 0x20 /* don't use a file */
#ifdef CONFIG_MMAP_ALLOW_UNINITIALIZED
#define MAP_UNINITIALIZED \
	0x4000000 /* For anonymous mmap, memory could be uninitialized */
#else
#define MAP_UNINITIALIZED 0x0 /* Don't support this flag */
#endif

#define VIRT_TO_PHY(x) mm_virt_to_phys((vaddr_t)(x))
#define PHY_TO_VIRT(x) mm_phys_to_virt((paddr_t)(x))
#define PHY_TO_PAGE_IDX(x) (((x) & PAGE_SIZE_MASK) / PAGE_SIZE)
#define VIRT_TO_PAGE_IDX(x) PHY_TO_PAGE_IDX(VIRT_TO_PHY(x))

// clang-format on

extern unsigned phymm_end;
extern unsigned phymm_begin;
extern char g_cmdline[];
extern unsigned long long gdt[];
extern unsigned short gdt_size;
extern unsigned long intr_stubs[];
extern unsigned long long idt[];
extern unsigned short idt_size;

vaddr_t mm_get_pagedir(void);

// map 0xCxxxxxxxx to xxxxxxx
// return (is used for page table)
int mm_kmap_page(vaddr_t vir);

/* Map a physical page into the kernel physical-page alias space.
 * Safe for any 32-bit physical address; does not touch the allocator. */
int mm_kmap_phys(paddr_t phys);
void mm_kunmap_phys(paddr_t phys);

int mm_map_io(paddr_t phy);

vaddr_t mm_phys_to_virt(paddr_t phys);
paddr_t mm_virt_to_phys(vaddr_t virt);

void mm_kunmap_page(vaddr_t vir);

void mm_del_user_map();
void mm_destroy_user_map(vaddr_t page_dir);

vaddr_t mm_alloc_page_table(void);

void mm_free_page_table(vaddr_t vir);

vaddr_t vm_alloc(int page_count);

void vm_free(vaddr_t vm, int page_count);

int mm_map_page(vaddr_t vir, paddr_t phy, unsigned flag);

int mm_map_page_io(vaddr_t vir, paddr_t phy, unsigned flag);

void mm_unmap_page(vaddr_t vir);

pfn_t mm_get_attached_page_index(vaddr_t vir);

unsigned int mm_get_free_phy_page_index();

unsigned mm_get_map_flag(vaddr_t vir);
unsigned mm_get_map_flag_pd(vaddr_t page_dir, vaddr_t vir);

void mm_set_map_flag(vaddr_t vir, unsigned flag);
void mm_set_map_flag_pd(vaddr_t page_dir, vaddr_t vir, unsigned flag);

void mm_set_phy_page_mask(unsigned int page_index, unsigned int used);

int do_mmap(vaddr_t addr, unsigned int len, unsigned int prot,
	    unsigned int flags, int fd, unsigned int offset);

void do_mmap_update(vaddr_t addr, unsigned int prot, unsigned int flags);

vaddr_t do_mmap_kernel(vaddr_t addr, size_t len, unsigned int prot,
		      unsigned int flags, file *fp, unsigned int offset);

int do_munmap(void *addr, unsigned length);

void *name_get();

void name_put(void *name);

void mm_init_cache();
void mm_init_process_page_dir(vaddr_t page_dir);

#endif
