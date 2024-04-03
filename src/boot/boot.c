#include "klib.h"
#include <multiboot.h>
#include <config.h>
#include <mm.h>
#include <int.h>
#include <macro.h>
#include <phymm.h>

#define GET_BOOT_ADDR(type, addr) ((type)((unsigned)addr - KERNEL_OFFSET))
#define _START __attribute__((section(".multiboot")))

extern void syscall_handler();
extern void kmain_startup();

multiboot_info_t *g_mb;
unsigned long long gdt[SELECTOR_COUNT];
unsigned short gdt_size = sizeof(gdt);
unsigned long long idt[IDT_SIZE];
unsigned short idt_size = sizeof(idt);

#define OUT_PORT(port, data)                 \
	asm volatile("mov $" #port ", %dx"); \
	asm volatile("mov $" #data ", %al"); \
	asm volatile("outb %al, %dx");

_START static void init_interrupt()
{
	OUT_PORT(0x20, 0x11);
	OUT_PORT(0xA0, 0x11);
	OUT_PORT(0x21, 0x20);
	OUT_PORT(0xA1, 0x28);

	OUT_PORT(0x21, 0x4);
	OUT_PORT(0xA1, 0x2);

	OUT_PORT(0x21, 0x1);
	OUT_PORT(0xA1, 0x1);

	// disable all interrupt in this time,
	// enable them after all setup done
	OUT_PORT(0x21, 0xFF);
	OUT_PORT(0xA1, 0xFF);
}

_START static void setup_gdt()
{
	unsigned short ds = KERNEL_DATA_SELECTOR;
	unsigned long long *_gdt = GET_BOOT_ADDR(unsigned long long *, gdt);
	unsigned long long operand = 0;
	_gdt[0] = 0;
	_gdt[KERNEL_CODE_SELECTOR / 8] =
		MAKE_SEG_DESC(0, ADDRESS_LIMIT, SEG_CLASS_DATA, 10,
			      KERNEL_PRIVILEGE, SEG_BASE_4K);
	_gdt[KERNEL_DATA_SELECTOR / 8] =
		MAKE_SEG_DESC(0, ADDRESS_LIMIT, SEG_CLASS_DATA, 2,
			      KERNEL_PRIVILEGE, SEG_BASE_4K);
	_gdt[USER_CODE_SELECTOR / 8] =
		MAKE_SEG_DESC(0, ADDRESS_LIMIT, SEG_CLASS_DATA, 8,
			      USER_PRIVILEGE, SEG_BASE_4K);
	_gdt[USER_DATA_SELECTOR / 8] =
		MAKE_SEG_DESC(0, ADDRESS_LIMIT, SEG_CLASS_DATA, 2,
			      USER_PRIVILEGE, SEG_BASE_4K);
	_gdt[TSS_SELECTOR / 8] = MAKE_SEG_DESC(0, 0x67, SEG_CLASS_SYSTEM, 9,
					       KERNEL_PRIVILEGE,
					       SEG_BASE_1); // tmp
	operand = MAKE_GDTR_OPERAND(sizeof gdt - 1, _gdt);
	SET_GDT(operand);
	SET_CS(KERNEL_CODE_SELECTOR);
	SET_DS(KERNEL_DATA_SELECTOR);
}

_START static void setup_idt()
{
	int i = 0;
	unsigned long long *_idt = GET_BOOT_ADDR(unsigned long long *, idt);
	unsigned long *_stubs = GET_BOOT_ADDR(unsigned long *, intr_stubs);
	_stubs[SYSCALL_INT_NO] = (unsigned)syscall_handler;
	for (i = 0; i < IDT_SIZE; i++) {
		_idt[i] = MAKE_INTR_GATE((void *)_stubs[i], 0);
	}
}

_START static void mm_setup_beginning_8m()
{
	unsigned int phy = PAGE_ENTRY_USER_DATA;
	unsigned int reserved_page_tables = (RESERVED_PAGES / 1024);
	unsigned int pg = (GDT_ADDRESS + PAGE_SIZE);
	unsigned int *pgt;
	int i = 0, j = 0;
	unsigned int *gdt = (int *)GDT_ADDRESS;

	for (i = 0; i < PG_TABLE_SIZE; i++) {
		gdt[i] = 0;
	}
	for (i = 0; i < reserved_page_tables; i++) {
		gdt[i] = (pg + i * PAGE_SIZE) | PAGE_ENTRY_USER_DATA;
		gdt[i + 768] = (pg + i * PAGE_SIZE) | PAGE_ENTRY_USER_DATA;
	}

	for (j = 0; j < reserved_page_tables; j++) {
		pgt = (unsigned int *)(pg + j * PAGE_SIZE);
		for (i = 0; i < PE_TABLE_SIZE; i++) {
			pgt[i] = phy;
			phy += PAGE_SIZE;
		}
	}

	return;
}

_START void mm_get_phy_mem_bound(multiboot_info_t *mb,
				 unsigned long long *volatile mem_low,
				 unsigned long long *volatile mem_high)
{
	unsigned int map_addr = mb->mmap_addr;
	unsigned long long low, high;
	memory_map_t *map;

	if (mb->flags & 0x40) {
		map = (memory_map_t *)mb->mmap_addr;
		while ((unsigned int)map < mb->mmap_addr + mb->mmap_length) {
			if (map->type == 0x1 && (map->base_addr_low != 0 ||
						 map->base_addr_high != 0)) {
				low = (unsigned long long)map->base_addr_low +
				      ((unsigned long long)map->base_addr_high
				       << 32);
				high = low +
				       ((unsigned long long)map->length_low +
					((unsigned long long)map->length_high
					 << 32));
				*mem_low = low;
				*mem_high = high;
				break;
			}
			map = (memory_map_t *)((unsigned int)map + map->size +
					       sizeof(unsigned int));
		}
	}
}

_START void boot_stage1(multiboot_info_t *mb, unsigned int magic)
{
	multiboot_info_t **dst = &g_mb;
	multiboot_info_t **src = &mb;
	unsigned long long mem_low, mem_high;
	if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		return;
	}

	*(multiboot_info_t **)((unsigned)dst - KERNEL_OFFSET) =
		(multiboot_info_t *)((unsigned)src + KERNEL_OFFSET);

	init_interrupt();

	setup_gdt();

	setup_idt();

	mm_get_phy_mem_bound(mb, &mem_low, &mem_high);

	mm_setup_beginning_8m();

	SET_CR3(GDT_ADDRESS);
	ENABLE_PAGING();

	// ok now we make eip as virtual address
	RELOAD_EIP();

	// ok now we make esp as virtual address
	RELOAD_ESP();

	phymm_max = (mem_high) / PAGE_SIZE;
	phymm_valid = phymm_get_mgmt_pages(mem_high);
	phymm_valid += (mem_low / PAGE_SIZE + RESERVED_PAGES);

	mm_init_page_table_cache();

	// physical memory management
	phymm_setup_mgmt_pages(mem_low / PAGE_SIZE + RESERVED_PAGES);

	kmain_startup();

	// never to here
	return;
}