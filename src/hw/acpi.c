/*
 * acpi.c — minimal ACPI MADT parser for SMP CPU enumeration.
 *
 * Scans firmware memory for the RSDP, follows the RSDT chain to the
 * MADT ("APIC") table, and extracts Local APIC IDs and the I/O APIC
 * physical address.
 *
 * All firmware structures live in the first 1 MB of physical memory,
 * which is mapped in the kernel's virtual address space at
 * KERNEL_OFFSET + physical_addr (established by stage1.c).
 */

#include <hw/acpi.h>
#include <mm/mm.h>
#include <lib/klib.h>

/*
 * Map a physical page (if not already mapped) and return its kernel virtual
 * address.  ACPI tables may live anywhere in physical RAM — including above
 * the initial 8 MB boot mapping — so we must ensure the page is mapped before
 * dereferencing.  Physical pages below PAGE_TABLE_CACHE_END - KERNEL_OFFSET
 * are already covered by the boot mapping; mm_kmap_phys() is a no-op for
 * those.
 */
static void *acpi_phys_to_virt(unsigned phys)
{
	mm_kmap_phys(phys & PAGE_SIZE_MASK);
	return (void *)(phys + KERNEL_OFFSET);
}

/* Convenience: plain virtual cast for addresses known to be in the
 * always-mapped first-8-MB range (EBDA, BIOS ROM). */
#define PHYS_TO_VIRT(p) ((void *)((unsigned)(p) + KERNEL_OFFSET))

/* 
 * RSDP discovery
 */

static int acpi_checksum(const void *buf, unsigned len)
{
	const unsigned char *p = buf;
	unsigned char sum = 0;
	unsigned i;
	for (i = 0; i < len; i++)
		sum += p[i];
	return sum == 0;
}

/* Search for "RSD PTR " in a physical range, aligned to 16 bytes. */
static acpi_rsdp_t *acpi_scan_range(unsigned phys_start, unsigned phys_end)
{
	unsigned addr;
	for (addr = phys_start; addr < phys_end; addr += 16) {
		acpi_rsdp_t *r = PHYS_TO_VIRT(addr);
		if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
		    acpi_checksum(r, 20))
			return r;
	}
	return NULL;
}

static acpi_rsdp_t *acpi_find_rsdp(void)
{
	acpi_rsdp_t *r;
	unsigned ebda_seg, ebda_phys;

	/* 1. Try EBDA (Extended BIOS Data Area): segment stored at 0x40E */
	ebda_seg = *(unsigned short *)PHYS_TO_VIRT(0x40E);
	ebda_phys = ebda_seg << 4;
	if (ebda_phys >= 0x80000 && ebda_phys < 0xA0000) {
		r = acpi_scan_range(ebda_phys, ebda_phys + 1024);
		if (r)
			return r;
	}

	/* 2. Try BIOS ROM area: 0xE0000 – 0xFFFFF */
	r = acpi_scan_range(0xE0000, 0x100000);
	return r; /* may be NULL */
}

/* 
 * MADT parsing
 */

static void acpi_parse_madt(acpi_madt_t *madt, acpi_info_t *info)
{
	unsigned char *p = (unsigned char *)(madt + 1);
	unsigned char *end = (unsigned char *)madt + madt->header.length;

	while (p < end) {
		acpi_madt_entry_t *entry = (acpi_madt_entry_t *)p;
		if (entry->length < 2)
			break;

		if (entry->type == ACPI_MADT_LAPIC) {
			acpi_madt_lapic_t *lapic = (acpi_madt_lapic_t *)p;
			/* bit 0 of flags: processor enabled */
			if ((lapic->flags & 1) && info->ncpus < MAX_CPUS) {
				info->apic_ids[info->ncpus] = lapic->apic_id;
				info->ncpus++;
			}
		} else if (entry->type == ACPI_MADT_IOAPIC) {
			acpi_madt_ioapic_t *ioapic = (acpi_madt_ioapic_t *)p;
			if (!info->ioapic_phys)
				info->ioapic_phys = ioapic->ioapic_addr;
		}

		p += entry->length;
	}
}

/* 
 * Public API
 */

int acpi_parse(acpi_info_t *info)
{
	acpi_rsdp_t *rsdp;
	acpi_rsdt_t *rsdt;
	unsigned *entries;
	int n, i;

	memset(info, 0, sizeof(*info));

	rsdp = acpi_find_rsdp();
	if (!rsdp) {
		printk("acpi: RSDP not found\n");
		return -1;
	}
	printk("acpi: RSDP at phys %x\n", (unsigned)rsdp - KERNEL_OFFSET);

	/* rsdt_addr can be anywhere in physical RAM; map before dereferencing. */
	rsdt = acpi_phys_to_virt(rsdp->rsdt_addr);
	if (memcmp(rsdt->header.signature, "RSDT", 4) != 0 ||
	    !acpi_checksum(rsdt, rsdt->header.length)) {
		printk("acpi: RSDT invalid\n");
		return -1;
	}

	n = (rsdt->header.length - sizeof(acpi_header_t)) / 4;
	entries = (unsigned *)(rsdt + 1);

	for (i = 0; i < n; i++) {
		/* Each child table pointer also needs mapping. */
		acpi_header_t *hdr = acpi_phys_to_virt(entries[i]);
		if (memcmp(hdr->signature, "APIC", 4) == 0 &&
		    acpi_checksum(hdr, hdr->length)) {
			printk("acpi: found MADT at phys 0x%x\n", entries[i]);
			acpi_parse_madt((acpi_madt_t *)hdr, info);
			printk("acpi: %d CPU(s), IOAPIC at 0x%x\n", info->ncpus,
			       info->ioapic_phys);
			return 0;
		}
	}

	printk("acpi: MADT not found\n");
	return -1;
}
