#ifndef _PS_INTERNAL_H_
#define _PS_INTERNAL_H_

/*
 * ps_internal.h — state and helpers shared across ps_*.c translation units.
 * Must NOT be included by code outside src/ps/.
 */

#include <ps/ps.h>
#include <lib/lock.h>
#include <lib/list.h>

/*
 * Scheduler control block
 */

typedef struct _ps_control {
	list_entry ready_queue[PS_PRIORITY_MAX];
	list_entry dying_queue;
	list_entry wait_queue;
	struct rb_root mgr_queue;
	int ps_count;
} ps_control;

/*
 * Shared globals (defined in ps.c)
 */

extern ps_control control;
extern spinlock_t ps_lock;
extern spinlock_t map_lock;
extern int _ps_enabled;
extern unsigned task_schedule_count;
extern tss_struct *tss_address;

/*
 * Cross-file non-public functions
 */

/* ps.c */
void ps_add_mgr_unsafe(task_struct *task);
void ps_add_mgr(task_struct *task);
void ps_remove_mgr_unsafe(task_struct *task);
void ps_remove_mgr(task_struct *task);
void reset_tss(task_struct *task);
unsigned ps_id_gen();
void ps_id_free(unsigned pid);

/* Set the kernel-mode segment selectors in a task's saved TSS.
 * Used by both ps_create and the fork helpers. */
static inline void task_init_selectors(task_struct *task)
{
	task->tss.fs = task->tss.gs = task->tss.ds = task->tss.es =
		task->tss.ss = KERNEL_DATA_SELECTOR;
	task->tss.cs = KERNEL_CODE_SELECTOR;
}

#endif /* _PS_INTERNAL_H_ */
