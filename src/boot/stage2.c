
#include <boot/multiboot.h>
#include <int/int.h>
#include <int/dsr.h>
#include <mm/mm.h>
#include <mm/pagefault.h>
#include <ps/ps.h>
#include <hw/serial.h>
#include <hw/vga.h>
#include <hw/time.h>
#include <hw/keyboard.h>
#include <hw/mouse.h>
#include <hw/tty.h>
#include <hw/font.h>
#include <lib/klib.h>

#include <macro.h>

static void run(void);
TEST_CONTROL TestControl;

static void kmain_process(void *param);
static void idle_process(void *param);
static void parse_kernel_cmdline();

/*
 * Kernel init-call table
 * Each entry is placed in ".kinit.<index>" by KERNEL_INIT(); the linker
 * script collects them in order between __kinit_start and __kinit_end.
 */
extern kinit_fn_t __kinit_start[];
extern kinit_fn_t __kinit_end[];

static void idle_process(void *param);

void kmain_startup()
{
	klib_init();

	font_init();

	fb_init();

	// after klib_init, kmalloc/kfree/prink/etc are workable
	tty_init();

	parse_kernel_cmdline();

	ps_init();

	dsr_init();

	int_enable_all();

	mm_del_user_map();

	serial_init_queue();

	kb_init();

	ps2mouse_init();

	time_init();

	time_calculate_cpu_cycle();

	pf_init();

	/* Mainline kernel runs uniprocessor only. */
	ps_create(idle_process, NULL, ps_idle, ps_kernel);
	// create first process
	ps_create(kmain_process, NULL, ps_normal, ps_kernel);

	ps_kickoff();

	run();
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
			TestControl.verbose = TEST_LOG_INFO;
		else if (strcmp(token, "verbose=0") == 0)
			TestControl.verbose = TEST_LOG_OFF;
		else if (strcmp(token, "verbose=1") == 0)
			TestControl.verbose = TEST_LOG_TRACE;
		else if (strcmp(token, "verbose=2") == 0)
			TestControl.verbose = TEST_LOG_INFO;
		else if (strcmp(token, "bash") == 0)
			TestControl.bash = 1;
		else if (strcmp(token, "test") == 0)
			TestControl.test = 1;

		token = end;
	} while (1);
}
