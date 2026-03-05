#include <timer.h>
#include <debugfs.h>
#include <mount.h>
#include <klib.h>
#include <int.h>
#include <keyboard.h>
#include <mm.h>
#include <multiboot.h>
#include <dsr.h>
#include <time.h>
#include <ps.h>
#include <block.h>
#include <pagefault.h>
#include <serial.h>
#include <vga.h>
#include <fs.h>
#include <macro.h>
#include <apic.h>
#include <acpi.h>
#include <cpu.h>

static void run(void);
TEST_CONTROL TestControl;

static void kmain_process(void *param);
static void timer_process(void *param);
static void idle_process(void *param);
static void parse_kernel_cmdline();

/* -----------------------------------------------------------------------
 * Kernel init-call table
 * Each entry is placed in ".kinit.<index>" by KERNEL_INIT(); the linker
 * script collects them in order between __kinit_start and __kinit_end.
 * ----------------------------------------------------------------------- */
extern kinit_fn_t __kinit_start[];
extern kinit_fn_t __kinit_end[];

/* ACPI result: populated during SMP init, used by cpu.c */
acpi_info_t g_acpi_info;

void kmain_startup()
{
	int i = 0;

	fb_init();

	fb_enable();

	// after klib_init, kmalloc/kfree/prink/etc are workable
	klib_init();

	printk("parse kernel command line\n");
	parse_kernel_cmdline();

	printk("Init process\n");
	ps_init();

	printk("Init dsr\n");
	dsr_init();

	printk("Enable interrupts\n");
	int_enable_all();

	mm_del_user_map();
	RELOAD_CR3();

	printk("Init serial\n");
	serial_init_queue();

	printk("Init keyboard\n");
	kb_init();

	printk("Init timer\n");
	time_init();

	printk("Caculate CPU caps\n");
	time_calculate_cpu_cycle();

	printk("Init page fault\n");
	pf_init();

	/* ---------------------------------------------------------------
	 * SMP: discover CPUs via ACPI, init LAPIC/IOAPIC, start APs.
	 * --------------------------------------------------------------- */
	printk("Init ACPI\n");
	if (acpi_parse(&g_acpi_info) == 0 && g_acpi_info.ncpus > 0) {
		unsigned ioapic_phys = g_acpi_info.ioapic_phys
					       ? g_acpi_info.ioapic_phys
					       : IOAPIC_BASE_PHY;

		printk("Init BSP LAPIC\n");
		apic_init_bsp();

		/* Switch interrupt EOI delivery to APIC (8259 now masked). */
		int_set_apic_mode();

		printk("Init IOAPIC\n");
		ioapic_init(ioapic_phys);

		/* Route hardware IRQs to BSP (APIC id 0). */
		ioapic_route(0,  INT_VECTOR_IRQ0,       g_acpi_info.apic_ids[0]);
		ioapic_route(1,  INT_VECTOR_IRQ0 + 1,   g_acpi_info.apic_ids[0]);
		ioapic_route(14, INT_VECTOR_IRQ0 + 14,  g_acpi_info.apic_ids[0]);
		ioapic_route(15, INT_VECTOR_IRQ0 + 15,  g_acpi_info.apic_ids[0]);

		printk("Init BSP CPU struct\n");
		cpu_init_bsp();

		printk("Start Application Processors\n");
		smp_start_aps();
	} else {
		printk("ACPI/SMP not available, running single-CPU\n");
	}

	printk("Start first process\n");

	// create idle process
	ps_create(idle_process, 1, ps_kernel);

	// create first process
	ps_create(kmain_process, 3, ps_kernel);

	// create timer process
	ps_create(timer_process, 3, ps_kernel);

	ps_create(dsr_process, 4, ps_dsr);

	ps_kickoff();

	run();
}

static void timer_process(void *param)
{
	timer_init();
	do_timer_loop();
}

static void idle_process(void *param)
{
	task_struct *cur = CURRENT_TASK();
	while (1) {
		HLT();
		task_sched();
	}
}

static void kmain_process(void *param)
{
	kinit_fn_t *fn;
	for (fn = __kinit_start; fn < __kinit_end; fn++)
		(*fn)();

	run();
}

static void run()
{
	idle_process(0);
}

static void parse_kernel_cmdline()
{
	char *cmd = g_cmdline;
	char *token, *end;
	token = cmd;
	memset(&TestControl, 0, sizeof(TestControl));

	if (strlen(g_cmdline) == 0) {
		return;
	}

#define SKIP_WHILE(t)                           \
	do {                                    \
		while (*t == ' ' || *t == '\t') \
			t++;                    \
	} while (0)

#define SKIP_CHAR(t)                                          \
	do {                                                  \
		while (*t != ' ' && *t != '\t' && *t != '\0') \
			t++;                                  \
	} while (0)

	do {
		SKIP_WHILE(token);
		end = token;
		SKIP_CHAR(end);
		if (*token == '\0')
			break;
		*end = '\0';
		end++;
		if (strcmp(token, "verbose") == 0)
			TestControl.verbos = 1;

		token = end;
	} while (1);
}
