/*
 * apic.c — Local APIC and I/O APIC driver.
 *
 * Local APIC (LAPIC): each CPU has one, provides per-CPU timers, IPIs,
 * and EOI.  MMIO base is at physical 0xFEE00000 by default.
 *
 * I/O APIC (IOAPIC): routes external hardware IRQs to CPUs.  MMIO base
 * is discovered from the ACPI MADT (usually physical 0xFEC00000).
 *
 * We map the LAPIC and IOAPIC MMIO pages into the kernel virtual address
 * space using mm_add_resource_map(), which maps physical P to virtual P
 * (valid since both addresses are above KERNEL_OFFSET = 0xC0000000).
 */

#include <apic.h>
#include <mm.h>
#include <klib.h>
#include <port.h>
#include <config.h>

/* -----------------------------------------------------------------------
 * LAPIC MMIO accessors
 * ----------------------------------------------------------------------- */

/* LAPIC is mapped at virtual == physical (both > 0xFEE00000). */
static volatile unsigned *lapic_base = (volatile unsigned *)LAPIC_BASE_PHY;

static inline unsigned lapic_read(unsigned reg)
{
	return lapic_base[reg / 4];
}

static inline void lapic_write(unsigned reg, unsigned val)
{
	lapic_base[reg / 4] = val;
	/* Fence: read SVR to serialise (LAPIC writes are not always ordered). */
	(void)lapic_base[LAPIC_SVR / 4];
}

/* -----------------------------------------------------------------------
 * Shared init sequence (BSP + AP)
 * ----------------------------------------------------------------------- */

/*
 * Virtual-wire mode: the BSP's LAPIC passes the 8259A PIC INTR signal
 * straight through via LINT0 (ExtINT delivery mode).  The PIC remains
 * active, so time_process continues to fire at vector 0x20 as before.
 * The LAPIC is used only for IPIs (TLB shootdown, scheduler kick).
 *
 * APs have their LINT0 masked since no PIC is wired to them.
 */
static void lapic_init_common(int is_bsp)
{
	/* Accept all interrupt priorities. */
	lapic_write(LAPIC_TPR, 0);

	/* Logical destination not used (physical destination only). */
	lapic_write(LAPIC_DFR, 0xFFFFFFFF);
	lapic_write(LAPIC_LDR, 0);

	/* LVT timer: masked (no per-CPU LAPIC timer in use). */
	lapic_write(LAPIC_LVT_TIMER, 0x00010000);

	/*
	 * LINT0:
	 *   BSP → ExtINT (delivery mode 7 = 0x700): the LAPIC forwards the
	 *          8259A PIC's INTR signal to the CPU transparently.
	 *   AP  → masked (APs are not connected to the 8259A).
	 */
	if (is_bsp)
		lapic_write(LAPIC_LVT_LINT0, 0x00000700); /* ExtINT, unmasked */
	else
		lapic_write(LAPIC_LVT_LINT0, 0x00010000); /* masked */

	/* LINT1: NMI on both BSP and APs (standard PC wiring). */
	lapic_write(LAPIC_LVT_LINT1, 0x00000400); /* NMI, unmasked */

	lapic_write(LAPIC_LVT_ERROR, 0x00010000); /* masked */

	/* Clear any pending error status. */
	lapic_write(LAPIC_ESR, 0);
	lapic_write(LAPIC_ESR, 0);

	lapic_write(LAPIC_EOI, 0);

	/* Software-enable the LAPIC. */
	lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | IPI_VECTOR_SPURIOUS);
}

/* -----------------------------------------------------------------------
 * BSP initialisation
 * ----------------------------------------------------------------------- */

void apic_init_bsp(void)
{
	/* Map LAPIC MMIO page into kernel virtual space. */
	mm_add_resource_map(LAPIC_BASE_PHY);

	/*
	 * Virtual-wire mode: do NOT mask the 8259A PIC.  The BSP's LINT0 is
	 * configured as ExtINT so the PIC's interrupt signal passes through
	 * the LAPIC transparently.  time_process keeps firing at vector 0x20.
	 */
	lapic_init_common(1 /* is_bsp */);

	printk("apic: BSP LAPIC id=%u enabled (virtual-wire mode)\n",
	       apic_id());
}

/* -----------------------------------------------------------------------
 * AP initialisation (called from ap_init_c on each AP)
 * ----------------------------------------------------------------------- */

void apic_init_ap(void)
{
	lapic_init_common(0 /* is_bsp */);
}

/* -----------------------------------------------------------------------
 * EOI and ID
 * ----------------------------------------------------------------------- */

void apic_eoi(void)
{
	lapic_write(LAPIC_EOI, 0);
}

unsigned apic_id(void)
{
	return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

/* -----------------------------------------------------------------------
 * IPI helpers
 * ----------------------------------------------------------------------- */

/* Wait until the ICR delivery status bit clears. */
static void apic_wait_ipi_idle(void)
{
	int timeout = 100000;
	while ((lapic_read(LAPIC_ICR_LO) & ICR_PENDING) && timeout-- > 0)
		; /* spin */
}

void apic_send_ipi(unsigned dest_apic_id, unsigned vector)
{
	apic_wait_ipi_idle();
	lapic_write(LAPIC_ICR_HI, dest_apic_id << 24);
	lapic_write(LAPIC_ICR_LO, ICR_FIXED | ICR_ASSERT | ICR_EDGE |
					  ICR_DEST_FIELD | vector);
	apic_wait_ipi_idle();
}

void apic_send_init(unsigned dest_apic_id)
{
	apic_wait_ipi_idle();
	lapic_write(LAPIC_ICR_HI, dest_apic_id << 24);
	lapic_write(LAPIC_ICR_LO,
		    ICR_INIT | ICR_ASSERT | ICR_LEVEL | ICR_DEST_FIELD);
	apic_wait_ipi_idle();

	/* De-assert INIT */
	lapic_write(LAPIC_ICR_HI, dest_apic_id << 24);
	lapic_write(LAPIC_ICR_LO,
		    ICR_INIT | ICR_DEASSERT | ICR_LEVEL | ICR_DEST_FIELD);
	apic_wait_ipi_idle();
}

void apic_send_sipi(unsigned dest_apic_id, unsigned start_page)
{
	/* start_page is the trampoline physical address >> 12 (page number).
	 * Do NOT wait for ICR idle after the write: in QEMU TCG, ICR_PENDING
	 * stays set until the AP vCPU actually runs, which it cannot do while
	 * the BSP is spinning here.  Linux likewise skips the post-SIPI poll. */
	apic_wait_ipi_idle();
	lapic_write(LAPIC_ICR_HI, dest_apic_id << 24);
	lapic_write(LAPIC_ICR_LO, ICR_SIPI | ICR_ASSERT | ICR_EDGE |
					  ICR_DEST_FIELD | (start_page & 0xFF));
}

/* -----------------------------------------------------------------------
 * I/O APIC
 * ----------------------------------------------------------------------- */

static volatile unsigned *ioapic_base;

static unsigned ioapic_read(unsigned reg)
{
	ioapic_base[IOAPIC_REGSEL / 4] = reg;
	return ioapic_base[IOAPIC_WIN / 4];
}

static void ioapic_write(unsigned reg, unsigned val)
{
	ioapic_base[IOAPIC_REGSEL / 4] = reg;
	ioapic_base[IOAPIC_WIN / 4] = val;
}

void ioapic_init(unsigned ioapic_phys)
{
	unsigned ver, max_redir, i;

	/* Map the IOAPIC MMIO page. */
	mm_add_resource_map(ioapic_phys);
	ioapic_base = (volatile unsigned *)ioapic_phys;

	ver = ioapic_read(IOAPIC_REG_VER);
	max_redir = (ver >> 16) & 0xFF; /* max redirection entry */

	/* Mask all redirection entries. */
	for (i = 0; i <= max_redir; i++) {
		ioapic_write(IOAPIC_REDTBL(i), IOREDTBL_MASKED);
		ioapic_write(IOAPIC_REDTBL(i) + 1, 0);
	}

	printk("apic: IOAPIC at 0x%x, %u IRQs\n", ioapic_phys, max_redir + 1);
}

void ioapic_route(unsigned irq_num, unsigned vector, unsigned dest_apic_id)
{
	unsigned lo = IOREDTBL_DELIV_FIXED | (vector & 0xFF);
	unsigned hi = dest_apic_id << 24;

	ioapic_write(IOAPIC_REDTBL(irq_num) + 1, hi);
	ioapic_write(IOAPIC_REDTBL(irq_num), lo); /* unmasks the entry */
}
