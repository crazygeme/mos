#include <boot/multiboot.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <int/int.h>
#include <macro.h>
#include <config.h>

#define GET_BOOT_ADDR(type, addr) ((type)((unsigned)addr - KERNEL_OFFSET))
#define _START __attribute__((section(".multiboot")))

extern void syscall_handler();
extern void kmain_startup();

char g_cmdline[256] = { 0 };
unsigned long long gdt[SELECTOR_COUNT];
unsigned short gdt_size = sizeof(gdt);
unsigned long long idt[IDT_SIZE];
unsigned short idt_size = sizeof(idt);

_START static void init_interrupt()
{
	/* ICW1: Begin initialization; expect ICW4 to follow. (PIC1_CMD/PIC2_CMD, ICW1) */
	OUT_PORT(0x20, 0x11); /* PIC1_CMD, ICW1 */
	OUT_PORT(0xA0, 0x11); /* PIC2_CMD, ICW1 */

	/* ICW2: Set vector offsets so hardware IRQs map to 0x20..0x2F. (PICx_DATA, ICW2_x_OFFSET) */
	OUT_PORT(0x21, 0x20); /* PIC1_DATA, ICW2_MASTER_OFFSET */
	OUT_PORT(0xA1, 0x28); /* PIC2_DATA, ICW2_SLAVE_OFFSET */

	/* ICW3: Wire master/slave relationship (slave on master's IR2). (PICx_DATA, ICW3) */
	OUT_PORT(0x21, 0x04); /* PIC1_DATA, ICW3_MASTER_SLAVE_ON_IR2 */
	OUT_PORT(0xA1, 0x02); /* PIC2_DATA, ICW3_SLAVE_ID_IR2 */

	/* ICW4: Set 8086/88 mode (as opposed to 8080/MCS-80 mode). (PICx_DATA, ICW4_8086) */
	OUT_PORT(0x21, 0x01); /* PIC1_DATA, ICW4_8086 */
	OUT_PORT(0xA1, 0x01); /* PIC2_DATA, ICW4_8086 */

	/* Mask all IRQ lines for now; unmask later after full setup. (PICx_DATA, IRQ_MASK_ALL) */
	OUT_PORT(0x21, 0xFF); /* PIC1_DATA, IRQ_MASK_ALL */
	OUT_PORT(0xA1, 0xFF); /* PIC2_DATA, IRQ_MASK_ALL */
}

_START static void setup_gdt()
{
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
	unsigned int reserved_page_tables =
		(RESERVED_PAGES + PE_TABLE_SIZE - 1) / PE_TABLE_SIZE;
	unsigned int pg = (GDT_ADDRESS + PAGE_SIZE);
	unsigned int *pgt;
	int i = 0, j = 0;
	unsigned int *gdt = (unsigned int *)GDT_ADDRESS;

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

/*
 * Scan every type-1 (usable RAM) entry in the multiboot mmap.
 *   *mem_low  <- base of the first non-zero usable region (for RESERVED_PAGES
 *                placement; typically the start of the 1 MB+ region).
 *   *mem_high <- end of the highest usable region (total physical extent).
 */
_START void mm_get_phy_mem_bound(multiboot_info_t *mb,
				 unsigned long long *volatile mem_low,
				 unsigned long long *volatile mem_high)
{
	memory_map_t *map;
	unsigned long long base, top;

	*mem_low = 0;
	*mem_high = 0;

	if (!(mb->flags & 0x40))
		return;

	map = (memory_map_t *)mb->mmap_addr;
	while ((unsigned int)map < mb->mmap_addr + mb->mmap_length) {
		if (map->type == 1) {
			base = (unsigned long long)map->base_addr_low |
			       ((unsigned long long)map->base_addr_high << 32);
			top = base +
			      ((unsigned long long)map->length_low |
			       ((unsigned long long)map->length_high << 32));

			/* Track first non-zero region for mem_low */
			if (base != 0 && *mem_low == 0)
				*mem_low = base;

			/* Track the highest end address for mem_high */
			if (top > *mem_high)
				*mem_high = top;
		}
		map = (memory_map_t *)((unsigned int)map + map->size +
				       sizeof(unsigned int));
	}
}

_START void boot_stage1(multiboot_info_t *mb, unsigned int magic)
{
	char *cmdline = (g_cmdline - KERNEL_OFFSET);
	char *cmdline_src = (char *)mb->cmdline;
	unsigned long long mem_low, mem_high;
	unsigned mmap_addr, mmap_len;

	if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
		return;

	if (mb->cmdline)
		while ((*cmdline++ = *cmdline_src++) != '\0')
			;

	init_interrupt();
	setup_gdt();
	setup_idt();

	mm_get_phy_mem_bound(mb, &mem_low, &mem_high);

	/* Save mmap location before enabling paging; accessible later at
	 * mmap_addr + KERNEL_OFFSET once the initial 8 MB window is live. */
	mmap_addr = (mb->flags & 0x40) ? mb->mmap_addr : 0;
	mmap_len = (mb->flags & 0x40) ? mb->mmap_length : 0;

	mm_setup_beginning_8m();

	SET_CR3(GDT_ADDRESS);
	ENABLE_PAGING();

	RELOAD_EIP();
	RELOAD_ESP();

	phymm_end = (unsigned)(mem_high / PAGE_SIZE);
	phymm_begin = phymm_get_mgmt_pages((unsigned)mem_high);
	phymm_begin += (unsigned)(mem_low / PAGE_SIZE) + RESERVED_PAGES;

	mm_init_page_table_cache();

	/* Map management pages and zero the phymm_pages array */
	phymm_setup_mgmt_pages((unsigned)(mem_low / PAGE_SIZE) +
			       RESERVED_PAGES);

	/* Mark non-RAM holes reserved and build buddy free lists */
	phymm_init(mmap_addr, mmap_len);

	kmain_startup();

	/* never reached */
}
