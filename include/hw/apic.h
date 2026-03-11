#ifndef _HW_APIC_H_
#define _HW_APIC_H_

/* -----------------------------------------------------------------------
 * Local APIC (LAPIC) MMIO registers (offset from LAPIC base virtual addr).
 * Default physical base: 0xFEE00000.
 * ----------------------------------------------------------------------- */
#define LAPIC_BASE_PHY 0xFEE00000u

#define LAPIC_ID 0x020 /* Local APIC ID (read) */
#define LAPIC_VER 0x030 /* Local APIC version */
#define LAPIC_TPR 0x080 /* Task Priority Register */
#define LAPIC_EOI 0x0B0 /* End-Of-Interrupt (write 0) */
#define LAPIC_LDR 0x0D0 /* Logical Destination */
#define LAPIC_DFR 0x0E0 /* Destination Format */
#define LAPIC_SVR 0x0F0 /* Spurious Interrupt Vector */
#define LAPIC_ESR 0x280 /* Error Status */
#define LAPIC_ICR_LO 0x300 /* Interrupt Command (low 32 bits) */
#define LAPIC_ICR_HI 0x310 /* Interrupt Command (high 32 bits) */
#define LAPIC_LVT_TIMER 0x320 /* LVT Timer */
#define LAPIC_LVT_LINT0 0x350 /* LVT LINT0 */
#define LAPIC_LVT_LINT1 0x360 /* LVT LINT1 */
#define LAPIC_LVT_ERROR 0x370 /* LVT Error */
#define LAPIC_TIMER_ICR 0x380 /* Timer Initial Count */
#define LAPIC_TIMER_CCR 0x390 /* Timer Current Count */
#define LAPIC_TIMER_DCR 0x3E0 /* Timer Divide Config */

/* SVR bits */
#define LAPIC_SVR_ENABLE 0x100 /* APIC software enable */

/* ICR delivery modes */
#define ICR_FIXED 0x00000
#define ICR_LOWEST 0x00100
#define ICR_SMI 0x00200
#define ICR_NMI 0x00400
#define ICR_INIT 0x00500
#define ICR_SIPI 0x00600

/* ICR level / trigger */
#define ICR_ASSERT 0x04000
#define ICR_DEASSERT 0x00000
#define ICR_EDGE 0x00000
#define ICR_LEVEL 0x08000

/* ICR destination shorthand */
#define ICR_DEST_FIELD 0x000000
#define ICR_DEST_SELF 0x040000
#define ICR_DEST_ALL 0x080000
#define ICR_DEST_ALL_EXCL 0x0C0000

/* ICR status */
#define ICR_PENDING 0x01000 /* delivery status = send pending */

/* -----------------------------------------------------------------------
 * I/O APIC MMIO
 * Default physical base: 0xFEC00000.
 * ----------------------------------------------------------------------- */
#define IOAPIC_BASE_PHY 0xFEC00000u

#define IOAPIC_REGSEL 0x00 /* Register Select */
#define IOAPIC_WIN 0x10 /* I/O Window (data) */

/* I/O APIC indirect registers */
#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REG_ARB 0x02
#define IOAPIC_REDTBL(n) (0x10 + 2 * (n)) /* Redirection table entry n */

/* Redirection table entry flags */
#define IOREDTBL_MASKED 0x10000 /* interrupt masked */
#define IOREDTBL_LEVEL 0x08000 /* level triggered */
#define IOREDTBL_LOGICAL 0x00800 /* logical destination */
#define IOREDTBL_DELIV_FIXED 0x00000 /* fixed delivery */

/* -----------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------- */

/* BSP: initialise the local APIC (disable 8259, enable LAPIC, set up SVR).
 * Must be called with paging enabled and LAPIC MMIO mapped. */
void apic_init_bsp(void);

/* AP: initialise the local APIC on this CPU (called from ap_init_c). */
void apic_init_ap(void);

/* Send APIC End-Of-Interrupt to the local APIC of the current CPU. */
void apic_eoi(void);

/* Return the APIC ID of the current CPU (reads LAPIC MMIO). */
unsigned apic_id(void);

/* Send a fixed-delivery IPI to a specific APIC ID. */
void apic_send_ipi(unsigned apic_id, unsigned vector);

/* Send INIT IPI to target AP. */
void apic_send_init(unsigned dest_apic_id);

/* Send Startup IPI (SIPI) to target AP with given start page (vector). */
void apic_send_sipi(unsigned dest_apic_id, unsigned start_page);

/* I/O APIC: initialise and mask all entries. */
void ioapic_init(unsigned ioapic_phys);

/* Route IRQ irq_num → vector on the given APIC destination. */
void ioapic_route(unsigned irq_num, unsigned vector, unsigned dest_apic_id);

#endif /* _APIC_H_ */
