/*
 * ps.c — process lifecycle, initialisation, and task-management primitives.
 *
 * Owns:
 *   - Global scheduler state (control block, locks)
 *   - ps_init / CURRENT_TASK / ps_kickoff
 *   - Manager queue (all-tasks list) and process lookup
 *   - TSS management
 *   - User address-space enumeration and cleanup
 */

#include <ps/ps.h>
#include <hw/cpu.h>
#include <int/int.h>
#include <fs/vfs.h>
#include <fs/fs.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/mm.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <lib/list.h>
#include <lib/port.h>
#include <config.h>
#include <macro.h>
#include <errno.h>

#include "ps_internal.h"

/*
 * Global variables
 */

ps_control control;

/* Fine-grained locks — acquire in this order to avoid deadlock:
 *   dying_queue_lock → ps_lock
 *   wait_queue_lock  → ps_lock
 * map_lock is independent. */
spinlock_t ps_lock; /* guards ready_queue, dying_queue, wait_queue, mgr_queue */
spinlock_t map_lock; /* guards per-process page-table operations */

#define PID_MAX_DEFAULT 32768

static spinlock_t psid_lock;
static unsigned char pid_bitmap[PID_MAX_DEFAULT / 8]; /* 1 bit per PID */
static unsigned last_pid = (unsigned)-1; /* last successfully allocated PID */

int _ps_enabled = 0;

/* Scheduling instrumentation (updated by ps_sched.c). */
unsigned task_schedule_count = 0;

static tss_io_struct *tss_address_storage = 0;
tss_struct *tss_address = 0; /* alias used by reset_tss and ps_sched.c */

/*
 * Static helpers — manager queue
 */
void ps_add_mgr_unsafe(task_struct *task)
{
	struct rb_root *root = &control.mgr_queue;
	struct rb_node **link;
	struct rb_node *parent = NULL;

	link = &root->rb_node;
	while (*link) {
		task_struct *t = rb_entry(*link, task_struct, mgr_rb);
		parent = *link;
		if (task->psid < t->psid)
			link = &(*link)->rb_left;
		else if (task->psid > t->psid)
			link = &(*link)->rb_right;
		else {
			return; /* psid already present */
		}
	}
	rb_link_node(&task->mgr_rb, parent, link);
	rb_insert_color(&task->mgr_rb, root);
	control.ps_count++;
}

void ps_add_mgr(task_struct *task)
{
	int irq;
	spinlock_lock(&ps_lock, &irq);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

void ps_remove_mgr_unsafe(task_struct *task)
{
	if (!RB_EMPTY_NODE(&task->mgr_rb)) {
		rb_erase(&task->mgr_rb, &control.mgr_queue);
		RB_CLEAR_NODE(&task->mgr_rb);
	}
	control.ps_count--;
	ps_id_free(task->psid);
}

void ps_remove_mgr(task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	ps_remove_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

int ps_total_count()
{
	int ret = 0;
	int irq;

	spinlock_lock(&ps_lock, &irq);
	ret = control.ps_count;
	spinlock_unlock(&ps_lock, irq);
	return ret;
}

/*
 * Static helpers — TSS and context-switch support
 */

/* Reload the global TSS with the given task's CR3 and kernel stack pointer. */
void reset_tss(task_struct *task)
{
	tss_io_struct *io_tss = (tss_io_struct *)tss_address;

	tss_address->cr3 = task->cr3;
	tss_address->esp0 = task->tss.esp0;
	tss_address->iomap = (unsigned short)offsetof(tss_io_struct, io_bitmap);
	if (task->io_allow_all) {
		memset(io_tss->io_bitmap, 0x00, TSS_IO_BITMAP_BYTES);
	} else if (task->io_bitmap) {
		memcpy(io_tss->io_bitmap, task->io_bitmap, TSS_IO_BITMAP_BYTES);
	} else {
		memset(io_tss->io_bitmap, 0xff, TSS_IO_BITMAP_BYTES);
	}
	io_tss->io_bitmap[TSS_IO_BITMAP_BYTES] = 0xff;
	tss_address->ss0 = KERNEL_DATA_SELECTOR;
	tss_address->ss = tss_address->gs = tss_address->fs = tss_address->ds =
		tss_address->es = KERNEL_DATA_SELECTOR | 0x3;
	tss_address->cs = KERNEL_CODE_SELECTOR | 0x3;
	int_update_tss((unsigned int)tss_address);
}

int ps_set_ioperm(task_struct *task, unsigned long from, unsigned long num,
		  int turn_on)
{
	unsigned long end;
	unsigned long port;

	if (!task)
		return -EINVAL;
	if (num == 0)
		return 0;

	end = from + num;
	if (end <= from || end > (TSS_IO_BITMAP_BYTES * 8))
		return -EINVAL;
	if (!task->io_bitmap)
		return -ENOMEM;

	task->io_allow_all = 0;
	for (port = from; port < end; port++) {
		unsigned long byte = port >> 3;
		unsigned char bit = (unsigned char)(1U << (port & 0x7));

		if (turn_on)
			task->io_bitmap[byte] &= (unsigned char)~bit;
		else
			task->io_bitmap[byte] |= bit;
	}

	reset_tss(task);
	return 0;
}

static void ap_idle_stub(void *param)
{
	while (1) {
		HLT();
		task_sched();
	}
}

/*
 * Static helpers — user address-space enumeration
 */

static void ps_cleanup_enum_callback(void *aux, unsigned vir, unsigned phy)
{
	mm_unmap_page(vir);
}

/*
 * Private — process ID
 */

unsigned ps_id_gen()
{
	unsigned pid, start;
	int irq;

	spinlock_lock(&psid_lock, &irq);
	start = last_pid;
	pid = start + 1;
	if (pid >= PID_MAX_DEFAULT)
		pid = 1;

	while (pid != start) {
		if (!(pid_bitmap[pid / 8] & (1 << (pid % 8)))) {
			pid_bitmap[pid / 8] |= (1 << (pid % 8));
			last_pid = pid;
			spinlock_unlock(&psid_lock, irq);
			return pid;
		}
		if (++pid >= PID_MAX_DEFAULT)
			pid = 0;
	}

	spinlock_unlock(&psid_lock, irq);
	return (unsigned)-1; /* PID space exhausted */
}

void ps_id_free(unsigned pid)
{
	int irq;

	if (pid >= PID_MAX_DEFAULT)
		return;
	spinlock_lock(&psid_lock, &irq);
	pid_bitmap[pid / 8] &= ~(1 << (pid % 8));
	spinlock_unlock(&psid_lock, irq);
}

/*
 * Public — initialisation
 */

void ps_init()
{
	int i;
	list_init(&control.dying_queue);
	list_init(&control.wait_queue);
	control.mgr_queue.rb_node = NULL;
	control.timer_queue.rb_node = NULL;
	control.ps_count = 0;
	for (i = 0; i < PS_PRIORITY_MAX; i++)
		list_init(&control.ready_queue[i]);

	spinlock_init(&ps_lock);
	spinlock_init(&psid_lock);
	spinlock_init(&map_lock);

	_ps_enabled = 0;
	task_schedule_count = 0;

	tss_address_storage = kmalloc(sizeof(tss_io_struct));
	memset(tss_address_storage, 0xff, sizeof(tss_io_struct));
	tss_address = &tss_address_storage->tss;
	tss_address->iomap = (unsigned short)offsetof(tss_io_struct, io_bitmap);
	int_update_tss((unsigned int)tss_address);
}

int ps_enabled()
{
	return _ps_enabled;
}

/*
 * Public — current task and bootstrap
 */

/* Mark the bootstrap task as non-schedulable and enter the task queue. */
void ps_kickoff()
{
	task_struct *cur = CURRENT_TASK();
	memset(cur, 0, sizeof(*cur));
	cur->psid = 0xffffffff;
	cur->ps_list.prev = cur->ps_list.next = 0;
	cur->stats = NULL;
	_ps_enabled = 1;
	task_sched();
}

/* Called by each AP after per-CPU LAPIC/TSS setup. */
void ps_kickoff_ap(void)
{
	task_struct *cur = CURRENT_TASK();
	memset(cur, 0, sizeof(*cur));
	cur->psid = 0xffffffff;
	cur->ps_list.prev = cur->ps_list.next = 0;
	ps_create(ap_idle_stub, NULL, ps_idle, ps_kernel);
	_ps_enabled = 1;
	task_sched();
}

/*
 * Public — TSS management
 */

void ps_update_tss(unsigned int esp0)
{
	task_struct *task = CURRENT_TASK();
	task->tss.esp0 = esp0;
	reset_tss(task);
}

/*
 * Public — process lookup and enumeration
 */

/* RB-tree search of mgr_queue by psid. Returns NULL if not found. */
task_struct *ps_find_process_unsafe(unsigned psid)
{
	struct rb_node *node;
	task_struct *task = NULL;

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
	return task;
}

task_struct *ps_find_process(unsigned psid)
{
	task_struct *task = NULL;
	int irq;

	spinlock_lock(&ps_lock, &irq);
	task = ps_find_process_unsafe(psid);
	spinlock_unlock(&ps_lock, irq);
	return task;
}

/* Invoke callback(task, ctx) for every live task (in psid order). */
void ps_enum_all(ps_enum_callback callback, void *ctx)
{
	struct rb_node *node;
	int irq;

	if (!callback)
		return;

	spinlock_lock(&ps_lock, &irq);
	node = rb_first(&control.mgr_queue);
	while (node) {
		task_struct *task = rb_entry(node, task_struct, mgr_rb);
		spinlock_unlock(&ps_lock, irq);
		callback(task, ctx);
		spinlock_lock(&ps_lock, &irq);
		node = rb_next(node);
	}
	spinlock_unlock(&ps_lock, irq);
}

/* Send signal sig to every user task whose group_id matches pgrp. */
void ps_send_signal_pgrp(unsigned pgrp, int sig)
{
	struct rb_node *node;
	int irq;

	if (!pgrp || sig <= 0 || sig >= NSIG)
		return;

	spinlock_lock(&ps_lock, &irq);
	node = rb_first(&control.mgr_queue);
	while (node) {
		task_struct *task = rb_entry(node, task_struct, mgr_rb);
		node = rb_next(node);
		if (task->type == ps_user && task->user &&
		    task->user->group_id == pgrp) {
			task->signal->sig_pending |= (1UL << (sig - 1));
			if (task->status == ps_waiting &&
			    !(task->signal->sig_mask & (1UL << (sig - 1))))
				ps_put_to_ready_queue_unsafe(task);
		}
	}
	spinlock_unlock(&ps_lock, irq);
}

/*
 * Public — user address-space management
 */

/* Walk every mapped user page and invoke fn(aux, vir, phy) for each.
 * Only covers the user portion (indices 0..KERNEL_PAGE_DIR_OFFSET-1). */
void ps_enum_user_map(task_struct *task, fpuser_map_callback fn, void *aux)
{
	unsigned i, j;
	unsigned int *page_dir;

	if (!fn || !task->user->page_dir)
		return;

	page_dir = (unsigned int *)task->user->page_dir;
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
