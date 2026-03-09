/*
 * ps.c — process lifecycle, initialisation, and task-management primitives.
 *
 * Owns:
 *   - Global scheduler state (control block, locks)
 *   - ps_init / ps_create / CURRENT_TASK / ps_kickoff
 *   - Manager queue (all-tasks list) and process lookup
 *   - TSS management
 *   - User address-space enumeration and cleanup
 *   - Scheduler queries (ps_has_ready, ps_enum_all)
 */

#include "ps.h"
#include <cpu.h>
#include <mount.h>
#include <mmap.h>
#include <phymm.h>
#include <lock.h>
#include <mm.h>
#include <klib.h>
#include <config.h>
#include <int.h>
#include <list.h>
#include <macro.h>
#include <port.h>
#include <fs.h>
#include "ps_internal.h"

/* -------------------------------------------------------------------------
 * Global variables
 * ------------------------------------------------------------------------- */

ps_control control;

/* Fine-grained locks — acquire in this order to avoid deadlock:
 *   dying_queue_lock → ps_lock
 *   wait_queue_lock  → ps_lock
 * map_lock is independent. */
spinlock_t ps_lock; /* guards ready_queue, dying_queue, wait_queue, mgr_queue */
spinlock_t map_lock; /* guards per-process page-table operations */

static spinlock_t psid_lock; /* guards last_gen_pid counter */
static unsigned last_gen_pid = 0;

int _ps_enabled = 0;

/* Scheduling instrumentation (updated by ps_sched.c). */
unsigned long long task_schedule_time = 0;
unsigned task_schedule_count = 0;

static tss_struct *tss_address_storage = 0;
tss_struct *tss_address = 0; /* alias used by reset_tss and ps_sched.c */

/* -------------------------------------------------------------------------
 * Static helpers — manager queue
 * ------------------------------------------------------------------------- */

void ps_add_mgr(task_struct *task)
{
	struct rb_root *root = &control.mgr_queue;
	struct rb_node **link;
	struct rb_node *parent = NULL;

	spinlock_lock(&ps_lock);
	link = &root->rb_node;
	while (*link) {
		task_struct *t = rb_entry(*link, task_struct, mgr_rb);
		parent = *link;
		if (task->psid < t->psid)
			link = &(*link)->rb_left;
		else if (task->psid > t->psid)
			link = &(*link)->rb_right;
		else {
			spinlock_unlock(&ps_lock);
			return; /* psid already present */
		}
	}
	rb_link_node(&task->mgr_rb, parent, link);
	rb_insert_color(&task->mgr_rb, root);
	spinlock_unlock(&ps_lock);
}

void ps_remove_mgr(task_struct *task)
{
	spinlock_lock(&ps_lock);
	if (!RB_EMPTY_NODE(&task->mgr_rb)) {
		rb_erase(&task->mgr_rb, &control.mgr_queue);
		RB_CLEAR_NODE(&task->mgr_rb);
	}
	spinlock_unlock(&ps_lock);
}

/* -------------------------------------------------------------------------
 * Static helpers — TSS and context-switch support
 * ------------------------------------------------------------------------- */

/* Reload the global TSS with the given task's CR3 and kernel stack pointer. */
void reset_tss(task_struct *task)
{
	tss_address->cr3 = task->cr3;
	tss_address->esp0 = task->tss.esp0;
	tss_address->iomap = (unsigned short)sizeof(tss_struct);
	tss_address->ss0 = KERNEL_DATA_SELECTOR;
	tss_address->ss = tss_address->gs = tss_address->fs = tss_address->ds =
		tss_address->es = KERNEL_DATA_SELECTOR | 0x3;
	tss_address->cs = KERNEL_CODE_SELECTOR | 0x3;
	int_update_tss((unsigned int)tss_address);
}

/* -------------------------------------------------------------------------
 * Static helpers — task setup
 * ------------------------------------------------------------------------- */

static void ps_setup_task_frame(task_struct *task, unsigned data_seg,
				unsigned eax, unsigned ebx, unsigned ecx,
				unsigned edx, unsigned ebp, unsigned esp,
				unsigned esp0, unsigned eip)
{
	task->tss.fs = task->tss.gs = task->tss.es = task->tss.ss =
		task->tss.ds = data_seg;
	task->tss.cs = KERNEL_CODE_SELECTOR;
	task->tss.eax = eax;
	task->tss.ebx = ebx;
	task->tss.ecx = ecx;
	task->tss.edx = edx;
	task->tss.ebp = ebp;
	task->tss.esp = esp;
	task->tss.esp0 = esp0;
	task->tss.eip = eip;
}

/* Every kernel task begins here. Enables interrupts, runs the task function,
 * then moves the task to the dying queue and yields. */
static void ps_run()
{
	task_struct *task = CURRENT_TASK();
	process_fn fn;

	int_intr_enable();
	_ps_enabled = 1;
	task->status = ps_running;
	task->is_switching = 0;
	fn = task->fn;
	if (fn)
		fn(0);

	task->status = ps_dying;
	ps_put_to_dying_queue(task);
	ps_remove_mgr(task);
	task_sched();
}

static void ap_idle_stub(void *param)
{
	while (1) {
		HLT();
		task_sched();
	}
}

/* -------------------------------------------------------------------------
 * Static helpers — user address-space enumeration
 * ------------------------------------------------------------------------- */

static void ps_cleanup_enum_callback(void *aux, unsigned vir, unsigned phy)
{
	mm_del_dynamic_map(vir);
}

/* -------------------------------------------------------------------------
 * Private — process ID
 * ------------------------------------------------------------------------- */

unsigned ps_id_gen()
{
	return __sync_fetch_and_add(&last_gen_pid, 1);
}

/* -------------------------------------------------------------------------
 * Public — initialisation
 * ------------------------------------------------------------------------- */

void ps_init()
{
	int i;
	list_init(&control.dying_queue);
	list_init(&control.wait_queue);
	control.mgr_queue.rb_node = NULL;
	for (i = 0; i < PS_PRIORITY_MAX; i++)
		control.ready_queue[i].rb_node = NULL;

	spinlock_init(&ps_lock);
	spinlock_init(&psid_lock);
	spinlock_init(&map_lock);

	_ps_enabled = 0;
	task_schedule_time = 0;
	task_schedule_count = 0;

	tss_address_storage = kmalloc(sizeof(tss_struct));
	tss_address = tss_address_storage;
	int_update_tss((unsigned int)tss_address);
}

int ps_enabled()
{
	return _ps_enabled;
}

/* -------------------------------------------------------------------------
 * Public — task creation
 * ------------------------------------------------------------------------- */

/* Allocate and initialise a new kernel task. Returns the new psid, or
 * 0xffffffff on failure. The task is immediately placed in the ready queue. */
unsigned ps_create(process_fn fn, ps_priority priority, ps_type type)
{
	unsigned int stack_bottom;
	task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);

	if (priority >= PS_PRIORITY_MAX) {
		vm_free(task, 1);
		return 0xffffffff;
	}

	memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);

	task->user.page_dir = (unsigned int)vm_alloc(1);
	task->command = vm_alloc(1);
	task->umask = 0;
	strcpy(task->command, "system");
	memset(task->user.page_dir, 0, PAGE_SIZE);

	stack_bottom = (unsigned int)task + KERNEL_TASK_SIZE * PAGE_SIZE - 4;
	LOAD_CR3(task->cr3);
	task->ps_list.prev = task->ps_list.next = 0;
	RB_CLEAR_NODE(&task->mgr_rb);
	RB_CLEAR_NODE(&task->rb_node);
	task->sched_seq = 0;
	task->fn = fn;
	task->priority = priority;
	task->status = ps_ready;
	task->remain_ticks = DEFAULT_TASK_TIME_SLICE;
	task->timeout = 0;
	task->psid = ps_id_gen();
	task->is_switching = 0;
	task->fds = vm_alloc(1);
	task->cwd = name_get();
	memset(task->fds, 0, PAGE_SIZE);
	task->user.heap_top = USER_HEAP_BEGIN;
	task->user.vm = vm_create();
	memset(task->cwd, 0, MAX_PATH);
	mutex_init(&task->fd_lock);
	task->magic = 0xdeadbeef;

	ps_setup_task_frame(task, KERNEL_DATA_SELECTOR, 0, 0, 0, 0,
			    stack_bottom, stack_bottom, stack_bottom,
			    (unsigned)ps_run);
	ps_put_to_ready_queue(task);
	ps_add_mgr(task);
	return task->psid;
}

/* -------------------------------------------------------------------------
 * Public — current task and bootstrap
 * ------------------------------------------------------------------------- */

/* Derive the current task_struct from the stack pointer.
 * Each task occupies KERNEL_TASK_SIZE pages aligned to PAGE_SIZE. */
task_struct *CURRENT_TASK()
{
	task_struct *info = NULL;
	info = (task_struct *)(((unsigned int)&info) & PAGE_SIZE_MASK);
	return info;
}

/* Mark the bootstrap task as non-schedulable and enter the task queue. */
void ps_kickoff()
{
	task_struct *cur = CURRENT_TASK();
	cur->psid = 0xffffffff;
	cur->ps_list.prev = cur->ps_list.next = 0;
	_ps_enabled = 1;
	task_sched();
}

/* Called by each AP after per-CPU LAPIC/TSS setup. */
void ps_kickoff_ap(void)
{
	task_struct *cur = CURRENT_TASK();
	cur->psid = 0xffffffff;
	cur->ps_list.prev = cur->ps_list.next = 0;
	ps_create(ap_idle_stub, ps_idle, ps_kernel);
	_ps_enabled = 1;
	task_sched();
}

/* -------------------------------------------------------------------------
 * Public — TSS management
 * ------------------------------------------------------------------------- */

void ps_update_tss(unsigned int esp0)
{
	task_struct *task = CURRENT_TASK();
	task->tss.esp0 = esp0;
	reset_tss(task);
}

/* -------------------------------------------------------------------------
 * Public — process lookup and enumeration
 * ------------------------------------------------------------------------- */

/* RB-tree search of mgr_queue by psid. Returns NULL if not found. */
task_struct *ps_find_process(unsigned psid)
{
	struct rb_node *node;
	task_struct *task = NULL;

	spinlock_lock(&ps_lock);
	node = control.mgr_queue.rb_node;
	while (node) {
		task_struct *t = rb_entry(node, task_struct, mgr_rb);
		if (psid < t->psid)
			node = node->rb_left;
		else if (psid > t->psid)
			node = node->rb_right;
		else {
			task = t;
			break;
		}
	}
	spinlock_unlock(&ps_lock);
	return task;
}

/* Return non-zero if any task at or above the given priority level is ready. */
int ps_has_ready(int priority)
{
	int i;
	if (priority >= PS_PRIORITY_MAX || priority < 0)
		return 0;
	for (i = priority; i < PS_PRIORITY_MAX; i++) {
		if (!RB_EMPTY_ROOT(&control.ready_queue[i]))
			return 1;
	}
	return 0;
}

/* Invoke callback for every live task (in psid order). */
void ps_enum_all(ps_enum_callback callback)
{
	struct rb_node *node;
	if (!callback)
		return;
	node = rb_first(&control.mgr_queue);
	while (node) {
		task_struct *task = rb_entry(node, task_struct, mgr_rb);
		callback(task);
		node = rb_next(node);
	}
}

/* -------------------------------------------------------------------------
 * Public — user address-space management
 * ------------------------------------------------------------------------- */

/* Walk every mapped user page and invoke fn(aux, vir, phy) for each.
 * Only covers the user portion (indices 0..KERNEL_PAGE_DIR_OFFSET-1). */
void ps_enum_user_map(task_struct *task, fpuser_map_callback fn, void *aux)
{
	unsigned i, j;
	unsigned int *page_dir;

	if (!fn || !task->user.page_dir)
		return;

	page_dir = (unsigned int *)task->user.page_dir;
	for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
		unsigned *page_table =
			(unsigned *)(page_dir[i] & PAGE_SIZE_MASK);
		if (!page_table)
			continue;
		page_table = (unsigned *)((unsigned)page_table + KERNEL_OFFSET);
		for (j = 0; j < 1024; j++) {
			if ((page_table[j] & PAGE_SIZE_MASK) == 0)
				continue;
			unsigned vir = (i << 22) + (j << 12);
			unsigned phy = page_table[j] & PAGE_SIZE_MASK;
			fn(aux, vir, phy);
		}
	}
}

/* Unmap all user pages for task and flush the TLB. */
void ps_cleanup_all_user_map(task_struct *task)
{
	ps_enum_user_map(task, ps_cleanup_enum_callback, 0);
	RELOAD_CR3();
}
