
#include <boot/multiboot.h>
#include <int/int.h>
#include <int/dsr.h>
#include <mm/mm.h>
#include <mm/pagefault.h>
#include <ps/ps.h>
#include <hw/apic.h>
#include <hw/acpi.h>
#include <hw/cpu.h>
#include <hw/serial.h>
#include <hw/vga.h>
#include <hw/time.h>
#include <hw/keyboard.h>
#include <hw/tty.h>
#include <lib/timer.h>
#include <lib/klib.h>

#include <macro.h>

static void run(void);
TEST_CONTROL TestControl;

static void kmain_process(void *param);
static void timer_process(void *param);
static void idle_process(void *param);
static void parse_kernel_cmdline();

/*
 * Kernel init-call table
 * Each entry is placed in ".kinit.<index>" by KERNEL_INIT(); the linker
 * script collects them in order between __kinit_start and __kinit_end.
 */
extern kinit_fn_t __kinit_start[];
extern kinit_fn_t __kinit_end[];

/* ACPI result: populated during SMP init, used by cpu.c */
acpi_info_t g_acpi_info;

static void idle_process(void *param);

void kmain_startup()
{
	fb_init();

	fb_enable();

	// after klib_init, kmalloc/kfree/prink/etc are workable
	tty_init();
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

	/*
	 * SMP: discover CPUs via ACPI, init LAPIC/IOAPIC, start APs.
	 */
	printk("Init ACPI\n");
	if (acpi_parse(&g_acpi_info) == 0 && g_acpi_info.ncpus > 1) {
		unsigned ioapic_phys = g_acpi_info.ioapic_phys ?
					       g_acpi_info.ioapic_phys :
					       IOAPIC_BASE_PHY;

		printk("Init BSP LAPIC\n");
		apic_init_bsp();

		/*
		 * Virtual-wire mode: the 8259A PIC remains active and its INTR
		 * signal passes through BSP LINT0 (ExtINT).  Do NOT call
		 * int_set_apic_mode() — that would switch EOI to LAPIC, causing
		 * the 8259A to never receive its EOI and blocking all future PIC
		 * interrupts (including the PIT timer at vector 0x20).
		 *
		 * The IOAPIC is initialised (all entries masked) for IPI routing
		 * only; external IRQs stay with the 8259A.
		 */
		printk("Init IOAPIC\n");
		ioapic_init(ioapic_phys);

		printk("Init BSP CPU struct\n");
		cpu_init_bsp();

		printk("Start Application Processors\n");
		smp_start_aps();
	} else {
		printk("SMP not available, running single-CPU\n");
	}

	printk("Start first process\n");

	if (ncpus == 1) {
		// create idle process for single CPU system
		ps_create(idle_process, NULL, ps_idle, ps_kernel);
	}
	// create first process
	ps_create(kmain_process, NULL, ps_normal, ps_kernel);

	// create timer process
	ps_create(timer_process, NULL, ps_normal, ps_kernel);

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
		else if (strcmp(token, "profile") == 0)
			TestControl.profiling = 1;
		else if (strstr(token, "init=") == token)
			TestControl.init_binary = strdup(token + 5);

		token = end;
	} while (1);
}
