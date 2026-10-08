#include <boot/multiboot.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <int/interrupt.h>
#include <macro.h>
#include <lib/klib.h>
#include <config.h>
#include <ps/smp.h>
char g_cmdline[256];
unsigned long long gdt[SELECTOR_COUNT] = {
	[1] = 0x00af9a000000ffffULL, [2] = 0x00cf92000000ffffULL,
	[3] = 0x00cffa000000ffffULL, [4] = 0x00cff2000000ffffULL,
	[5] = 0x00affa000000ffffULL,
};
unsigned short gdt_size = sizeof(gdt);
unsigned long long idt[IDT_SIZE * 2];
unsigned short idt_size = sizeof(idt);
extern void kmain_startup(void);
void mm_get_phy_mem_bound(multiboot_info_t *mb,
			  unsigned long long *volatile mem_low,
			  unsigned long long *volatile mem_high)
{
	memory_map_t *map;
	unsigned long long base, top;

	*mem_low = 0;
	*mem_high = 0;

	if (!(mb->flags & 0x40))
		return;

	map = (memory_map_t *)(KERNEL_OFFSET + mb->mmap_addr);
	while ((uintptr_t)map <
	       KERNEL_OFFSET + mb->mmap_addr + mb->mmap_length) {
		if (map->type == 1) {
			base = (unsigned long long)map->base_addr_low |
			       ((unsigned long long)map->base_addr_high << 32);
			top = base +
			      ((unsigned long long)map->length_low |
			       ((unsigned long long)map->length_high << 32));
			if (base >= PHYMM_ADDRESS_LIMIT)
				goto next;
			if (top > PHYMM_ADDRESS_LIMIT)
				top = PHYMM_ADDRESS_LIMIT;

			/* Track first non-zero region for mem_low */
			if (base != 0 && *mem_low == 0)
				*mem_low = base;

			/* Track the highest end address for mem_high */
			if (top > *mem_high)
				*mem_high = top;
		}
next:
		map = (memory_map_t *)((uintptr_t)map + map->size +
				       sizeof(unsigned int));
	}
}

void boot_stage1(uint32_t info_physical, uint32_t magic)
{
	multiboot_info_t *mb = (void *)(KERNEL_OFFSET + info_physical);
	unsigned long long low, high;
	if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
		for (;;)
			HLT();
	if ((mb->flags & 4) && mb->cmdline) {
		const char *source = (void *)(KERNEL_OFFSET + mb->cmdline);
		unsigned i = 0;
		while (i + 1 < sizeof(g_cmdline) && source[i]) {
			g_cmdline[i] = source[i];
			i++;
		}
		g_cmdline[i] = 0;
	}
	/* Mask IRQs before installing the long-mode interrupt gates. */
	OUT_PORT(0x20, 0x11);
	OUT_PORT(0xa0, 0x11);
	OUT_PORT(0x21, 0x20);
	OUT_PORT(0xa1, 0x28);
	OUT_PORT(0x21, 4);
	OUT_PORT(0xa1, 2);
	OUT_PORT(0x21, 1);
	OUT_PORT(0xa1, 1);
	OUT_PORT(0x21, 0xff);
	OUT_PORT(0xa1, 0xff);
	for (unsigned i = 0; i < IDT_SIZE; i++)
		arch_interrupt_set_gate(i, intr_stubs[i], 0, 0);
	smp_bootstrap();
	mm_get_phy_mem_bound(mb, &low, &high);
	phymm_end = high / PAGE_SIZE;
	phymm_begin = phymm_get_mgmt_pages(phymm_end) + low / PAGE_SIZE +
		      RESERVED_PAGES;
	mm_init_cache();
	phymm_setup_mgmt_pages(low / PAGE_SIZE + RESERVED_PAGES);
	phymm_init((mb->flags & 0x40) ? mb->mmap_addr : 0,
		   (mb->flags & 0x40) ? mb->mmap_length : 0);
	kmain_startup();
	for (;;)
		HLT();
}
