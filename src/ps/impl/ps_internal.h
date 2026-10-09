#ifndef _PS_INTERNAL_H_
#define _PS_INTERNAL_H_

/*
 * ps_internal.h — state and helpers shared across ps_*.c translation units.
 * Must NOT be included by code outside src/ps/.
 */

#include <ps/ps.h>
#include <ps/task.h>
#include <lib/lock.h>
#include <lib/list.h>

/*
 * Scheduler control block
 */

typedef struct _ps_control {
	list_entry ready_queue[PS_PRIORITY_MAX];
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

/* Requires ps_lock across selection and task activation. */
task_struct *ps_get_next_task_unsafe(void);

/* ps.c */
void ps_add_mgr_unsafe(task_struct *task);
void ps_add_mgr(task_struct *task);
void ps_remove_mgr_unsafe(task_struct *task);
void ps_remove_mgr(task_struct *task);
void reset_tss(task_struct *task);
unsigned ps_id_gen();
void ps_id_free(unsigned pid);
signal_handlers *ps_copy_sighand(signal_handlers *old);
void ps_alarm_release_thread_group(task_thread *process);
int ps_clone_resources(task_struct *, task_struct *, unsigned long flags);
void ps_free_signal_queue(list_entry *);

/* ps_sched.c — timer helpers (called under ps_lock) */
void timer_arm_unsafe(task_struct *task, unsigned ms);
void timer_disarm_unsafe(task_struct *task);
void ps_fire_timers_unsafe(void);
void ps_alarm_disarm_unsafe(task_struct *task);
int ps_futex_wake_locked(task_memory *user, int *uaddr, int max_wake);
void ps_futex_remove_task_locked(task_struct *task);
void ps_clear_child_tid(task_struct *task);
void ps_release_robust_list(task_struct *task);

/* Shared task-creation helpers. */
int ps_dup_fds(task_struct *cur, task_struct *task, int share);
int do_vfork(unsigned long child_stack, unsigned long flags);
task_struct *fork_alloc_child(task_struct *cur);
void ps_copy_thread_state(task_struct *cur, task_struct *task);
int fork_dup_io(task_struct *cur, task_struct *task);
int fork_set_meta(task_struct *cur, task_struct *task, unsigned fork_flag);
void fork_enqueue(task_struct *cur, task_struct *task);
void ps_enqueue_child_first(task_struct *cur, task_struct *task);
void fork_abort_child(task_struct *task);
int copy_page_range(task_struct *parent, task_struct *child);
int ps_set_thread_area_for(task_struct *task, void *info);
int ps_set_clone_tls_for(task_struct *task, void *info,
			 const intr_frame *parent_frame);
void ps_stop_current(intr_frame *frame, int sig);
int ps_ptrace_maybe_stop_syscall(intr_frame *frame, int entering);

/* Set the kernel-mode segment selectors in a task's saved TSS.
 * Used by both ps_create and the fork helpers. */
static inline void task_init_selectors(task_struct *task)
{
	arch_task_init(task);
}

#endif /* _PS_INTERNAL_H_ */
