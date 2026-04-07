/*
 * cpu.c — per-CPU initialisation and SMP bring-up.
 *
 * Responsibilities:
 *   - Maintain the global cpus[] array (one entry per logical CPU).
 *   - Copy the AP trampoline to physical AP_TRAMPOLINE_PHYS.
 *   - Populate the AP params page at physical AP_PARAMS_PHYS.
 *   - Temporarily restore the low-memory identity mapping so the AP can
 *     safely enable paging before jumping to high virtual space.
 *   - Send INIT + SIPI + SIPI to each AP and wait for it to come online.
 *   - ap_init_c(): the first C function executed on each AP.
 */

#include <hw/cpu.h>
#include <hw/apic.h>
#include <hw/acpi.h>
#include <ps/ps.h>
#include <mm/mm.h>
#include <int/int.h>
#include <lib/klib.h>
#include <macro.h>
#include <config.h>

/* 
 * Global per-CPU state
 */

cpu_struct cpus[MAX_CPUS];
volatile int ncpus = 1; /* BSP is CPU 0, counted at boot */

/* 
 * AP trampoline and params layout
 */

/* Symbols exported from ap_trampoline.S */
extern char ap_trampoline_start[];
extern char ap_trampoline_end[];

/*
 * AP params page layout at physical AP_PARAMS_PHYS.
 * The trampoline accesses it both at physical (before paging) and at
 * AP_PARAMS_PHYS + KERNEL_OFFSET (after paging).
 */
typedef struct __attribute__((packed)) {
	unsigned cr3; /* page directory physical address */
	unsigned stack_top; /* per-AP kernel stack virtual address */
	unsigned init_fn; /* virtual address of ap_init_c */
	unsigned cpu_id; /* logical CPU id for this AP */
} ap_params_t;

#define AP_PARAMS_VIRT ((ap_params_t *)(AP_PARAMS_PHYS + KERNEL_OFFSET))

/* Per-AP kernel stacks: one 4 KB page per AP (cpu_id 1..MAX_CPUS-1). */
static unsigned char ap_stacks[MAX_CPUS][PAGE_SIZE]
	__attribute__((aligned(PAGE_SIZE)));

/* 
 * cpu_current / cpu_current_id
 */

cpu_struct *cpu_current(void)
{
	unsigned id = apic_id();
	int i;
	for (i = 0; i < ncpus; i++) {
		if (cpus[i].apic_id == id)
			return &cpus[i];
	}
	return &cpus[0]; /* fallback */
}

int cpu_current_id(void)
{
	return cpu_current()->cpu_id;
}

int cpu_id(int index)
{
	if (index < 0 || index >= ncpus)
		return -1;
	return cpus[index].cpu_id;
}

/* 
 * TLB shootdown IPI
 */

void smp_tlb_shootdown(void)
{
	int i;
	unsigned my_apic = apic_id();

	for (i = 0; i < ncpus; i++) {
		if (cpus[i].online && cpus[i].apic_id != my_apic)
			apic_send_ipi(cpus[i].apic_id, IPI_VECTOR_TLB);
	}
}

/* 
 * Per-CPU TSS management
 */

/* Update the GDT entry for cpu_id's TSS and load the TR register. */
static void cpu_load_tss(int cpu_id, tss_struct *tss)
{
	unsigned base = (unsigned)tss;
	extern unsigned long long gdt[];
	unsigned sel = TSS_SELECTOR_FOR(cpu_id);

	gdt[sel / 8] = MAKE_SEG_DESC(base, TSS_SEG_LIMIT, SEG_CLASS_SYSTEM, 9,
				     KERNEL_PRIVILEGE, SEG_BASE_1);
	SET_TSS(sel);
}

/* 
 * AP C-level initialisation (runs on each AP after paging is on)
 */

void ap_init_c(int cpu_id)
{
	cpu_struct *cpu = &cpus[cpu_id];

	/* Initialise this CPU's local APIC. */
	apic_init_ap();

	/* Set up per-CPU TSS. */
	cpu->tss = kmalloc(sizeof(tss_io_struct));
	memset((void *)cpu->tss, 0xff, sizeof(tss_io_struct));
	cpu->tss->ss0 = KERNEL_DATA_SELECTOR;
	cpu->tss->iomap = (unsigned short)offsetof(tss_io_struct, io_bitmap);
	cpu_load_tss(cpu_id, cpu->tss);

	/* Enable interrupts so the scheduler can run. */
	int_enable_all_ap();

	/* Mark this CPU as online and join the scheduler. */
	cpu->online = 1;
	__sync_synchronize();

	printk("cpu%d: online (APIC id %d)\n", cpu_id, cpu->apic_id);

	/* Create this CPU's idle task and kick off scheduling. */
	ps_kickoff_ap();
}

/* 
 * BSP: bring up all Application Processors
 */

/* Busy-wait for approximately @ms milliseconds using a simple loop.
 * Calibration not needed for the coarse delays used in APIC start-up. */
static void cpu_mdelay(unsigned ms)
{
	volatile unsigned i;
	for (i = 0; i < ms * 10000; i++)
		PAUSE();
}

/*
 * smp_start_aps — called once from stage2 to bring APs online.
 *
 * For each AP discovered by ACPI:
 *   1. Populate the AP params page.
 *   2. Send INIT, wait 10 ms.
 *   3. Send SIPI, wait 200 µs.  Repeat once.
 *   4. Poll cpu->online for up to 1 s.
 */
void smp_start_aps(void)
{
	extern acpi_info_t g_acpi_info;
	int i;
	unsigned trampoline_size;
	unsigned *pde;

	if (g_acpi_info.ncpus <= 1) {
		printk("smp: single CPU, skipping AP bring-up\n");
		return;
	}

	/* Copy trampoline to physical AP_TRAMPOLINE_PHYS.
	 * Physical 0x8000 is at virtual KERNEL_OFFSET + 0x8000. */
	trampoline_size = (unsigned)(ap_trampoline_end - ap_trampoline_start);
	memcpy((void *)(AP_TRAMPOLINE_PHYS + KERNEL_OFFSET),
	       ap_trampoline_start, trampoline_size);

	/* Temporarily restore the identity mapping for virtual 0–4 MB
	 * so the AP can safely enable paging then jump to high virtual.
	 * The kernel page directory lives at physical GDT_ADDRESS;
	 * its virtual address is GDT_ADDRESS + KERNEL_OFFSET.
	 * PDE[768] holds the kernel mapping of phys 0–4 MB → virt 0xC0000000.
	 * Setting PDE[0] = PDE[768] adds the identity alias. */
	pde = (unsigned *)(GDT_ADDRESS + KERNEL_OFFSET);
	pde[0] = pde[768];
	RELOAD_CR3();

	for (i = 1; i < g_acpi_info.ncpus && i < MAX_CPUS; i++) {
		unsigned apic = g_acpi_info.apic_ids[i];
		volatile int timeout;
		ap_params_t *params = AP_PARAMS_VIRT;

		/* Register this CPU. */
		cpus[i].cpu_id = i;
		cpus[i].apic_id = apic;
		cpus[i].online = 0;
		cpus[i].idle = NULL;

		/* Fill params page. */
		params->cr3 = GDT_ADDRESS; /* BSP page dir physical addr */
		params->stack_top = (unsigned)&ap_stacks[i][PAGE_SIZE];
		params->init_fn = (unsigned)ap_init_c;
		params->cpu_id = i;
		__sync_synchronize();

		printk("smp: starting CPU %d (APIC id %d)\n", i, apic);

		/* INIT IPI */
		apic_send_init(apic);
		cpu_mdelay(10);

		/* SIPI #1 */
		apic_send_sipi(apic, AP_TRAMPOLINE_PHYS >> 12);
		cpu_mdelay(1);

		/* SIPI #2 (spec says send twice if AP doesn't start) */
		if (!cpus[i].online) {
			apic_send_sipi(apic, AP_TRAMPOLINE_PHYS >> 12);
			cpu_mdelay(1);
		}

		/* Wait for AP to set cpu->online. */
		timeout = 1000; /* ~1 second */
		while (!cpus[i].online && timeout-- > 0)
			cpu_mdelay(1);

		if (!cpus[i].online) {
			printk("smp: CPU %d did not start\n", i);
			continue;
		}
		__sync_fetch_and_add(&ncpus, 1);
	}

	/* Remove the identity mapping now that all APs are in high virtual. */
	pde[0] = 0;
	RELOAD_CR3();

	printk("smp: %d CPU(s) online\n", ncpus);
}

/* 
 * BSP per-CPU init (called once for CPU 0)
 */

void cpu_init_bsp(void)
{
	cpus[0].cpu_id = 0;
	cpus[0].apic_id = apic_id();
	cpus[0].online = 1;
	cpus[0].tss = NULL; /* TSS for BSP is managed by ps_init() */
	cpus[0].idle = NULL;
}
