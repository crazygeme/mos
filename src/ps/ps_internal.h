#ifndef _PS_INTERNAL_H_
#define _PS_INTERNAL_H_

/*
 * ps_internal.h — state and helpers shared across ps_*.c translation units.
 * Must NOT be included by code outside src/ps/.
 */

#include <ps/ps.h>
#include <lib/lock.h>
#include <lib/list.h>

typedef struct _intr_frame intr_frame;

/*
 * Scheduler control block
 */

typedef struct _ps_control {
	list_entry ready_queue[PS_PRIORITY_MAX];
	list_entry dying_queue;
	list_entry wait_queue;
	struct rb_root mgr_queue;
	int ps_count;
	struct rb_root
		timer_queue; /* tasks sleeping in time_wait(), ordered by timer_due_ms */
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

task_struct *ps_get_next_task();

/* ps.c */
void ps_add_mgr_unsafe(task_struct *task);
void ps_add_mgr(task_struct *task);
void ps_remove_mgr_unsafe(task_struct *task);
void ps_remove_mgr(task_struct *task);
void reset_tss(task_struct *task);
unsigned ps_id_gen();
void ps_id_free(unsigned pid);

/* ps_sched.c — timer helpers (called under ps_lock) */
void timer_arm_unsafe(task_struct *task, unsigned ms);
void timer_disarm_unsafe(task_struct *task);
void ps_fire_timers_unsafe(void);
int ps_futex_wake_locked(user_enviroment *user, int *uaddr, int max_wake);
void ps_clear_child_tid(task_struct *task);

/* Shared task-creation helpers. */
void ps_dup_fds(task_struct *cur, task_struct *task);
int do_vfork(void);
task_struct *fork_alloc_child(task_struct *cur);
void fork_dup_user_env(task_struct *cur, task_struct *task);
void fork_dup_signal(task_struct *cur, task_struct *task);
int fork_dup_io(task_struct *cur, task_struct *task);
void fork_set_meta(task_struct *cur, task_struct *task, unsigned fork_flag);
void fork_enqueue(task_struct *cur, task_struct *task);
void copy_page_range(task_struct *parent, task_struct *child);
int ps_set_thread_area_for(task_struct *task, void *info);
int ps_read_process_memory(task_struct *task, const void *addr, void *dst,
			   unsigned len);
int ps_write_process_memory(task_struct *task, void *addr, const void *src,
			    unsigned len);
void ps_stop_current(intr_frame *frame, int sig);
void ps_ptrace_maybe_stop_syscall(intr_frame *frame, int entering);

/* Set the kernel-mode segment selectors in a task's saved TSS.
 * Used by both ps_create and the fork helpers. */
static inline void task_init_selectors(task_struct *task)
{
	task->tss.fs = task->tss.gs = task->tss.ds = task->tss.es =
		task->tss.ss = KERNEL_DATA_SELECTOR;
	task->tss.cs = KERNEL_CODE_SELECTOR;
}

#endif /* _PS_INTERNAL_H_ */
