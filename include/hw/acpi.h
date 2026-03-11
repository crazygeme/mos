#ifndef _HW_ACPI_H_
#define _HW_ACPI_H_

#include <config.h>

/* -----------------------------------------------------------------------
 * ACPI table structures (packed, as they appear in firmware memory).
 * ----------------------------------------------------------------------- */

/* RSDP (Root System Description Pointer) */
typedef struct __attribute__((packed)) {
	char signature[8]; /* "RSD PTR " */
	unsigned char checksum;
	char oemid[6];
	unsigned char revision;
	unsigned int rsdt_addr; /* physical address of RSDT */
	/* ACPI 2.0+ fields follow, but we only use RSDT */
} acpi_rsdp_t;

/* Generic ACPI table header */
typedef struct __attribute__((packed)) {
	char signature[4];
	unsigned int length;
	unsigned char revision;
	unsigned char checksum;
	char oemid[6];
	char oem_table_id[8];
	unsigned int oem_revision;
	unsigned int creator_id;
	unsigned int creator_revision;
} acpi_header_t;

/* RSDT: contains an array of 32-bit physical addresses to other tables */
typedef struct __attribute__((packed)) {
	acpi_header_t header; /* signature "RSDT" */
	/* followed by variable number of uint32_t pointers */
} acpi_rsdt_t;

/* MADT (Multiple APIC Description Table): signature "APIC" */
typedef struct __attribute__((packed)) {
	acpi_header_t header;
	unsigned int lapic_addr; /* local APIC physical address */
	unsigned int flags; /* bit 0: dual 8259 PICs installed */
} acpi_madt_t;

/* MADT entry header */
typedef struct __attribute__((packed)) {
	unsigned char type;
	unsigned char length;
} acpi_madt_entry_t;

/* MADT entry type 0: Processor Local APIC */
#define ACPI_MADT_LAPIC 0
typedef struct __attribute__((packed)) {
	acpi_madt_entry_t hdr;
	unsigned char acpi_proc_id;
	unsigned char apic_id;
	unsigned int flags; /* bit 0: enabled */
} acpi_madt_lapic_t;

/* MADT entry type 1: I/O APIC */
#define ACPI_MADT_IOAPIC 1
typedef struct __attribute__((packed)) {
	acpi_madt_entry_t hdr;
	unsigned char ioapic_id;
	unsigned char reserved;
	unsigned int ioapic_addr; /* physical address */
	unsigned int gsi_base; /* global system interrupt base */
} acpi_madt_ioapic_t;

/* MADT entry type 2: Interrupt Source Override */
#define ACPI_MADT_ISO 2
typedef struct __attribute__((packed)) {
	acpi_madt_entry_t hdr;
	unsigned char bus;
	unsigned char source; /* IRQ source (ISA IRQ number) */
	unsigned int gsi; /* global system interrupt */
	unsigned short flags;
} acpi_madt_iso_t;

/* -----------------------------------------------------------------------
 * Result structure filled by acpi_parse()
 * ----------------------------------------------------------------------- */
typedef struct {
	unsigned char apic_ids[MAX_CPUS]; /* LAPIC IDs of found CPUs */
	int ncpus; /* number of CPUs found */
	unsigned ioapic_phys; /* physical address of first IOAPIC */
} acpi_info_t;

/* Scan firmware memory for the ACPI RSDP, parse RSDT/MADT, and populate
 * info.  Returns 0 on success, -1 if ACPI/MADT not found. */
int acpi_parse(acpi_info_t *info);

#endif /* _ACPI_H_ */
