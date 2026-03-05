#ifndef _MM_H_
#define _MM_H_
#include <config.h>

typedef struct multiboot_info multiboot_info_t;

#define KERNEL_OFFSET 0xC0000000
#define KERNEL_SIZE 0x40000000
#define PG_TABLE_SIZE 1024
#define PE_TABLE_SIZE 1024
#define KERNEL_PAGE_DIR_OFFSET (KERNEL_OFFSET / (4 * 1024 * 1024))

#define OFFSET_IN_PAGE_MASK 0x00000FFF
#define OFFSET_IN_PET_MASK 0x003FF000
#define OFFSET_IN_PGT_MASK 0xFFC00000

#define ADDR_TO_PGT_OFFSET(addr) ((addr & OFFSET_IN_PGT_MASK) >> 22)
#define ADDR_TO_PET_OFFSET(addr) ((addr & OFFSET_IN_PET_MASK) >> 12)
#define ADDR_TO_PAGE_OFFSET(addr) (addr & OFFSET_IN_PAGE_MASK)

#define USABLE_PG_INDEX_MIN 2
#define USABLE_PG_INDEX_MAX 1023

#define PAGE_ENTRY_PRESENT 0x01 // present if set
#define PAGE_ENTRY_WRITABLE 0x02 // writable if set
#define PAGE_ENTRY_DPL_USER 0x04 // user can access if set
#define PAGE_ENTRY_WT 0x08 // write through if set
#define PAGE_ENTRY_CD 0x10 // cache disable if set

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

// clang-format on

extern unsigned phymm_end;
extern unsigned phymm_begin;
extern char g_cmdline[];
extern unsigned long long gdt[];
extern unsigned short gdt_size;
extern unsigned long intr_stubs[];
extern unsigned long long idt[];
extern unsigned short idt_size;

unsigned mm_get_pagedir();

// map 0xCxxxxxxxx to xxxxxxx
// return (is used for page table)
int mm_add_direct_map(unsigned int vir);

int mm_add_resource_map(unsigned int phy);

void mm_del_direct_map(unsigned int vir);

void mm_del_user_map();

extern void vm_page_inval();

unsigned int mm_alloc_page_table();

void mm_free_page_table(unsigned int vir);

unsigned int vm_alloc(int page_count);

void vm_free(unsigned int vm, int page_count);

int mm_add_dynamic_map(unsigned int vir, unsigned int phy, unsigned flag);

void mm_del_dynamic_map(unsigned int vir);

unsigned mm_get_attached_page_index(unsigned int vir);

unsigned int mm_get_free_phy_page_index();

unsigned mm_get_map_flag(unsigned vir);

void mm_set_map_flag(unsigned vir, unsigned flag);

void mm_set_phy_page_mask(unsigned int page_index, unsigned int used);

unsigned vm_get_usr_zone(unsigned page_count);

int do_mmap(unsigned int addr, unsigned int len, unsigned int prot,
	    unsigned int flags, int fd, unsigned int offset);

int do_mmap_kernel(unsigned int addr, unsigned int len, unsigned int prot,
		   unsigned int flags, void *inode, unsigned int offset);

int do_munmap(void *addr, unsigned length);

void *name_get();

void name_put(void *name);

void mm_init_page_table_cache();

/* Callback type invoked when a new kernel page-directory entry is allocated.
 * Registered by ps.c so the scheduler can propagate the entry to all existing
 * task page directories without creating a mm→ps dependency. */
typedef void (*kernel_pde_propagator_t)(unsigned offset, unsigned value);
void mm_set_kernel_pde_propagator(kernel_pde_propagator_t fn);

/* Copy the kernel portion of the global kernel page directory (entries
 * KERNEL_PAGE_DIR_OFFSET..1023) into @pgd.  Call this when initialising a
 * new task's page directory so it inherits all current kernel mappings. */
void mm_copy_kernel_pgd(unsigned int *pgd);

#endif
