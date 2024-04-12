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
#include <hdd.h>
#include <syscall.h>
#include <pagefault.h>
#include <console.h>
#include <kbchar.h>
#include <null.h>
#include <pipechar.h>
#include <serial.h>
#include <vga.h>
#include <tests.h>
#include <fs.h>
#include <macro.h>

static void run(void);
TEST_CONTROL TestControl;

static void kmain_process(void *param);
static void timer_process(void *param);
static void idle_process(void *param);
static void parse_kernel_cmdline();

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

extern void nic_scan_all();
extern void user_first_process_run();

static void kmain_process(void *param)
{
	klog_init();

	printk("Init block devices\n");
	block_init();

	hdd_init();

	printk("Mount root fs to \"/\" \n");
	fs_mount_root();

	kbchar_init();

	console_init();

	null_init();

	pipe_init();

	debugfs_init();

	// enable network
	nic_scan_all();

	printk("Init system call table\n");
	syscall_init();

	klib_clear();

	user_first_process_run();

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
