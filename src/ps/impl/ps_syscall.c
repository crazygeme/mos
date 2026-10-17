/*
 * ps_syscall.c — process syscalls: exit, wait, getcwd; and shutdown.
 *
 * Owns:
 *   - Process exit (do_exit, sys_exit)
 *   - Child reaping (do_waitpid, sys_waitpid)
 *   - sys_getcwd
 *   - System shutdown (reboot, shutdown)
 */

#include <ps/ps.h>
#include <ps/namespace.h>
#include <ps/cgroup.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <int/int.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/list.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/port.h>
#include <device/blockdev.h>
#include <driver/driver.h>
#include <config.h>
#include <macro.h>
#include <errno.h>
#include <ext4.h>

#include "ps_internal.h"
#include <ps/smp.h>
#include <ps/usage.h>

#define W_STOPCODE(sig) (((sig) << 8) | 0x7f)

/*
 * Static helpers — child reaping
 */

static void ps_reap_task(task_struct *task, rusage *rusage)
{
	while (__atomic_load_n(&task->sched->enumeration_refs,
			       __ATOMIC_ACQUIRE))
		time_wait(1);
	unsigned long long child_utime =
		ps_usage_read(&task->thread->user_tickets) +
		ps_usage_read(&task->thread->child_utime);
	unsigned long long child_stime =
		ps_usage_read(&task->thread->kernel_tickets) +
		ps_usage_read(&task->thread->child_stime);

	if (rusage) {
		memset(rusage, 0, sizeof(*rusage));
		rusage->ru_majflt = task->stats->pf_major;
		rusage->ru_minflt = task->stats->pf_minor;
		rusage->ru_nvcsw =
			task->stats->total_switches - task->stats->niv_switches;
		rusage->ru_nivcsw = task->stats->niv_switches;
		us_to_timeval(child_stime * (1000000ULL / HZ),
			      &rusage->ru_stime);
		us_to_timeval(child_utime * (1000000ULL / HZ),
			      &rusage->ru_utime);
	}

	/* Accumulate child CPU time into parent for cutime/cstime. */
	if (!(task->life->fork_flag & FORK_FLAG_THREAD)) {
		int irq;
		spinlock_lock(&ps_lock, &irq);
		task_struct *parent = ps_find_process_unsafe(task->life->ppid);
		if (parent && parent->execution && parent->thread) {
			ps_usage_add_local(&parent->thread->child_utime,
					   child_utime);
			ps_usage_add_local(&parent->thread->child_stime,
					   child_stime);
		}
		spinlock_unlock(&ps_lock, irq);
	}

	ps_free_task(task);
}

static int is_child_unsafe(task_struct *parent, task_struct *task)
{
	return task->life->parent_group == parent->thread;
}

static task_struct *find_exited_child_unsafe(task_struct *parent, unsigned pid,
					     unsigned pgrp, int match_pgrp)
{
	list_entry *head = &parent->thread->children;
	for (list_entry *entry = head->next; entry != head;
	     entry = entry->next) {
		task_struct *task =
			container_of(entry, task_schedule, child_list)->task;
		if (!task->life->exit_ready || task->sched->on_cpu ||
		    task->thread->live_tasks)
			continue;
		if (pid && task->life->psid != pid)
			continue;
		if (match_pgrp && task->thread->group_id != pgrp)
			continue;
		return task;
	}
	return NULL;
}

static void detach_exited_child_unsafe(task_struct *task)
{
	list_remove_entry(&task->sched->ps_list);
	ps_remove_mgr_unsafe(task);
}

static int has_child_unsafe(task_struct *parent, unsigned pid)
{
	if (pid) {
		task_struct *task = ps_find_process_unsafe(pid);
		return task && is_child_unsafe(parent, task);
	}
	return !list_is_empty(&parent->thread->children);
}

static task_struct *find_stopped_child_unsafe(task_struct *parent, unsigned pid,
					      int options)
{
	list_entry *head = &parent->thread->children;
	for (list_entry *entry = head->next; entry != head;
	     entry = entry->next) {
		task_struct *task =
			container_of(entry, task_schedule, child_list)->task;
		if (pid && task->life->psid != pid)
			continue;
		if (!task->life->stop_report_pending)
			continue;
		if (!(options & WUNTRACED) &&
		    (!task->execution ||
		     task->execution->ptrace_tracer != parent->life->psid))
			continue;
		return task;
	}
	return NULL;
}

static int has_pgrp_child_unsafe(task_struct *parent, unsigned pgrp)
{
	list_entry *head = &parent->thread->children;
	for (list_entry *entry = head->next; entry != head;
	     entry = entry->next) {
		task_struct *task =
			container_of(entry, task_schedule, child_list)->task;
		if (task->thread->group_id == pgrp)
			return 1;
	}
	return 0;
}

/*
 * Static helpers — system shutdown
 */

static void close_fp_callback(task_struct *task, void *ctx)
{
	(void)ctx;
	ps_put_fds(task);
}

static void system_down(int process)
{
	klog_close();
	if (process)
		ps_enum_all(close_fp_callback, NULL);
	ext4_umount("/");
	blockdev_close_all();
}

void qemu_exit(unsigned char code)
{
	port_write_byte(0xf4, code);
}

/* PPid names the parent thread group. Only its final task transfers children. */
void ps_reparent_children_unsafe(task_struct *cur)
{
	int notify_init = 0;
	while (!list_is_empty(&cur->life->children)) {
		task_struct *child = container_of(cur->life->children.next,
						  task_schedule, creator_list)
					     ->task;
		if (child->life->pdeath_signal && child->sighand &&
		    child->sched->status != ps_dying)
			ps_queue_group_signal_unsafe(
				child, child->life->pdeath_signal);
		list_remove_entry(&child->sched->creator_list);
		list_init(&child->sched->creator_list);
		child->life->creator = NULL;
	}
	cur->life->nchildren = 0;
	if (cur->thread->live_tasks || list_is_empty(&cur->thread->children))
		return;
	unsigned namespace_init = ps_namespace_reaper(cur);
	task_struct *init_task = ps_find_process_unsafe(namespace_init);
	unsigned parent_pid = cur->life->ppid;
	for (unsigned depth = 0; depth < 512; depth++) {
		task_struct *parent = ps_find_process_unsafe(parent_pid);
		if (!parent || parent == cur)
			break;
		if (parent->thread->child_subreaper &&
		    parent->sched->status != ps_dying &&
		    (namespace_init == 1 ||
		     ps_pid_for(cur, parent->thread->tgid))) {
			init_task = parent;
			break;
		}
		if (parent->thread->tgid == namespace_init ||
		    parent->life->ppid == parent_pid)
			break;
		parent_pid = parent->life->ppid;
	}
	if (!init_task || init_task->thread == cur->thread)
		return;
	while (!list_is_empty(&cur->thread->children)) {
		task_struct *child = container_of(cur->thread->children.next,
						  task_schedule, child_list)
					     ->task;
		ps_set_parent_unsafe(child, init_task);
		if (child->life->exit_ready) {
			list_remove_entry(&child->sched->ps_list);
			list_insert_tail(&init_task->life->dying_queue,
					 &child->sched->ps_list);
			notify_init = 1;
		}
	}
	if (notify_init) {
		ps_queue_group_signal_unsafe(init_task, SIGCHLD);
		ps_wake_parent_waiters_unsafe(init_task);
	}
}

void ps_reparent_children(task_struct *cur)
{
	int irq;
	spinlock_lock(&ps_lock, &irq);
	ps_reparent_children_unsafe(cur);
	spinlock_unlock(&ps_lock, irq);
}

static void ps_cancel_io_wait(task_struct *task)
{
	/* Detach I/O waiters before closing files or freeing the kernel stack. */
	if (task->wait->cancel_io_wait) {
		void (*cancel)(void *) = task->wait->cancel_io_wait;
		void *wait = task->wait->io_wait;
		task->wait->cancel_io_wait = NULL;
		task->wait->io_wait = NULL;
		cancel(wait);
	}
	fs_cancel_io(task);
}

static void ps_reap_group_thread(task_struct *task)
{
	if (!task)
		return;

	ps_cancel_io_wait(task);

	ps_clear_child_tid(task);
	ps_release_robust_list(task);

	ps_put_fds(task);

	ps_reap_task(task, NULL);
}

static list_entry dead_threads = { &dead_threads, &dead_threads };
static task_struct *dead_reaper;
static int dead_reaper_waiting;

static void ps_reap_dead_threads(void)
{
	int irq;
	for (;;) {
		task_struct *task = NULL;
		spinlock_lock(&ps_lock, &irq);
		for (list_entry *node = dead_threads.next;
		     node != &dead_threads; node = node->next) {
			task_struct *candidate =
				(container_of(node, task_schedule, ps_list)
					 ->task);
			if (!candidate->sched->on_cpu &&
			    !candidate->sched->enumeration_refs) {
				task = candidate;
				list_remove_entry(node);
				list_init(node);
				break;
			}
		}
		spinlock_unlock(&ps_lock, irq);
		if (!task)
			return;
		ps_reap_group_thread(task);
	}
}

static void ps_notify_reaper_unsafe(void)
{
	if (dead_reaper && dead_reaper_waiting) {
		dead_reaper_waiting = 0;
		ps_put_to_ready_queue_unsafe(dead_reaper);
	}
}

/* Resource destruction may block and must own an independent task context. */
static void ps_dead_reaper(void *param)
{
	int irq;
	(void)param;
	for (;;) {
		ps_reap_dead_threads();
		spinlock_lock(&ps_lock, &irq);
		if (list_is_empty(&dead_threads)) {
			dead_reaper_waiting = 1;
			ps_put_to_wait_queue_unsafe(current, NULL);
			spinlock_unlock(&ps_lock, irq);
			task_sched();
		} else {
			/* Retain stacks still active on a CPU or under enumeration. */
			spinlock_unlock(&ps_lock, irq);
			time_wait(1);
		}
	}
}

void ps_reaper_start(void)
{
	unsigned pid = ps_create(ps_dead_reaper, NULL, ps_normal, ps_kernel);
	dead_reaper = ps_find_process(pid);
	if (!dead_reaper)
		DIE();
}

void ps_stop_terminated_task(void)
{
	int irq;
	spinlock_lock(&ps_lock, &irq);
	list_remove_entry(&current->sched->ps_list);
	list_init(&current->sched->ps_list);
	current->sched->status = ps_stopped;
	spinlock_unlock(&ps_lock, irq);
}

void ps_kill_thread_group(task_struct *leader, unsigned encoded_status)
{
	list_entry *entry, *next;
	list_entry reap_list;
	int irq;

	if (!leader)
		return;
	ps_timer_discard_group(leader->thread->tgid);

	list_init(&reap_list);

	/* Stop remote users before touching their VM, descriptors or stack.
	 * Release ps_lock before waiting so remote timer entries can stop tasks. */
	for (;;) {
		int active = 0;
		spinlock_lock(&ps_lock, &irq);
		list_entry *head = &leader->thread->members;
		for (entry = head->next; entry != head; entry = entry->next) {
			task_struct *task =
				container_of(entry, task_schedule, thread_list)
					->task;
			if (task != leader &&
			    task->thread->tgid == leader->thread->tgid &&
			    task->life->type == ps_user) {
				task->sched->terminate_requested = 1;
				active |= task->sched->on_cpu != 0 ||
					  task->sched->vm_lock_depth != 0;
			}
		}
		spinlock_unlock(&ps_lock, irq);
		if (!active)
			break;
		time_wait(1);
	}

	spinlock_lock(&ps_lock, &irq);
	list_entry *head = &leader->thread->members;
	for (entry = head->next; entry != head; entry = next) {
		task_struct *task =
			container_of(entry, task_schedule, thread_list)->task;
		next = entry->next;

		if (task == leader)
			continue;
		if (task->life->type != ps_user || !task->sighand)
			continue;
		if (task->thread->tgid != leader->thread->tgid)
			continue;

		/*
		 * exit_group() must remove sibling threads from every scheduler data
		 * structure before the leader destroys shared VM/file-table state.
		 */
		timer_disarm_unsafe(task);
		ps_futex_remove_task_unsafe(task);
		list_remove_entry(&task->sched->ps_list);
		ps_mark_dying_unsafe(task);
		if (task->life->fork_flag & FORK_FLAG_THREAD)
			ps_remove_mgr_unsafe(task);

		list_insert_tail(&reap_list, &task->sched->ps_list);
	}
	spinlock_unlock(&ps_lock, irq);

	while (!list_is_empty(&reap_list)) {
		task_struct *task =
			(container_of(reap_list.next, task_schedule, ps_list)
				 ->task);

		/* Cleanup may unlink the task again when publishing its exit. */
		list_remove_entry(&task->sched->ps_list);
		list_init(&task->sched->ps_list);
		if (task->life->fork_flag & FORK_FLAG_THREAD) {
			ps_reap_group_thread(task);
		} else {
			/* Retain the process leader as the parent's waitable zombie. */
			ps_cancel_io_wait(task);
			ps_clear_child_tid(task);
			ps_release_robust_list(task);
			ps_put_fds(task);
			task->life->exit_status = encoded_status;
			ps_put_to_dying_queue(task);
		}
	}
}

void do_group_exit(unsigned encoded_status)
{
	fs_posix_lock_release(NULL, CURRENT_TASK()->thread->tgid);
	ps_kill_thread_group(CURRENT_TASK(), encoded_status);
	do_exit(encoded_status);
}

void do_exit(unsigned encoded_status)
{
	task_struct *cur = CURRENT_TASK();

	ps_ptrace_stop_exit(encoded_status);
	cur->life->exit_status = encoded_status;
	if (!(cur->life->fork_flag & FORK_FLAG_THREAD)) {
		fs_posix_lock_release(NULL, cur->thread->tgid);
		ps_timer_discard_group(cur->thread->tgid);
	}
	if (TEST_LOG(TEST_LOG_INFO))
		klog("exit(%s, status=%x)\n", cur->memory->command,
		     encoded_status);

	if (cur->life->fork_flag & FORK_FLAG_VFORK) {
		cond_notify(&cur->life->vfork_event);
	}

	ps_cgroup_exit(cur);
	ps_namespace_exit(cur);
	ps_cancel_io_wait(cur);
	ps_clear_child_tid(cur);
	ps_release_robust_list(cur);

	if (cur->memory) {
		/* Flush dirty MAP_SHARED pages while user pages are still mapped. */
		vm_flush_all_dirty(cur->memory);
	}

	ps_put_fds(cur);

	if (cur->life->psid == 0) {
		printk("fatal error! process 0 exit\n");
		DIE();
	}
	if (cur->life->psid == 1) {
		if (TestControl.test) {
			unsigned char code =
				(unsigned char)((encoded_status >> 8) & 0xff);
			klog("test mode: init exited with status %u\n", code);
			int_intr_enable();
			system_down(1);
			int_intr_disable();
			qemu_exit(code);
			for (;;)
				HLT();
		}
		shutdown();
	}

	if (cur->life->fork_flag & FORK_FLAG_THREAD) {
		int irq;
		spinlock_lock(&ps_lock, &irq);
		task_struct *leader = ps_find_process_unsafe(cur->thread->tgid);
		ps_remove_mgr_unsafe(cur);
		if (!cur->thread->live_tasks && leader && leader != cur &&
		    leader->life->exit_ready) {
			task_struct *parent =
				ps_find_process_unsafe(leader->life->ppid);
			if (parent) {
				ps_child_signal_unsafe(parent, leader, SIGCHLD);
				ps_wake_parent_waiters_unsafe(parent);
			}
		}

		cur->life->psid = 0xffffffff;
		cur->sched->status = ps_dying;
		list_remove_entry(&cur->sched->ps_list);
		list_insert_tail(&dead_threads, &cur->sched->ps_list);
		ps_notify_reaper_unsafe();
		spinlock_unlock(&ps_lock, irq);
		task_sched();
	}

	/* ps_put_to_dying_queue queues SIGCHLD on the parent atomically. */
	ps_put_to_dying_queue(cur);

	task_sched();
}

int sys_exit(unsigned status)
{
	do_exit(status << 8);
	return 0;
}

/* Waitid event selection and snapshots are serialized with task transitions. */
int do_waitid(int type, unsigned id, struct mos_signal_info *info, int options,
	      rusage *usage)
{
	const unsigned supported = WNOHANG | WSTOPPED | WEXITED | WNOWAIT;
	task_struct *cur = current, *task;
	unsigned pid = 0, pgrp = 0;
	int irq, ret = 0, exited = 0;
	if (type < 0 || type > 2 || (options & ~supported) ||
	    !(options & (WEXITED | WSTOPPED)) || id > 0x7fffffffU ||
	    (type == 1 && !id))
		return -EINVAL;
	if (type == 1)
		pid = ps_pid_global(id);
	if (type == 2)
		pgrp = id ? ps_pid_global(id) : cur->thread->group_id;
	memset(info, 0, sizeof(*info));
	if (usage)
		memset(usage, 0, sizeof(*usage));
	spinlock_lock(&ps_lock, &irq);
	for (;;) {
		int children = 0;
		task = NULL;
		list_entry *head = &cur->thread->children;
		for (list_entry *entry = head->next; entry != head;
		     entry = entry->next) {
			task_struct *candidate =
				container_of(entry, task_schedule, child_list)
					->task;
			if ((type == 1 && candidate->life->psid != pid) ||
			    (type == 2 && candidate->thread->group_id != pgrp))
				continue;
			children = 1;
			exited = candidate->life->exit_ready &&
				 !candidate->sched->on_cpu &&
				 !candidate->thread->live_tasks;
			if (((options & WEXITED) && exited) ||
			    ((options & WSTOPPED) &&
			     candidate->life->stop_report_pending)) {
				task = candidate;
				break;
			}
		}
		if (task) {
			info->signo = SIGCHLD;
			info->pid = ps_pid_visible(task->life->psid);
			info->uid = task->credentials->uid;
			if (exited) {
				unsigned status = task->life->exit_status;
				info->code = !(status & 0x7f) ? 1 :
					     (status & 0x80)  ? 3 :
								2;
				info->status = status & 0x7f ?
						       status & 0x7f :
						       (status >> 8) & 0xff;
			} else {
				info->code =
					task->execution->ptrace_tracer ? 4 : 5;
				info->status = task->life->stop_signal;
			}
			info->utime =
				ps_usage_read(&task->thread->user_tickets);
			info->stime =
				ps_usage_read(&task->thread->kernel_tickets);
			if (usage) {
				usage->ru_majflt = task->stats->pf_major;
				usage->ru_minflt = task->stats->pf_minor;
				usage->ru_nvcsw = task->stats->total_switches -
						  task->stats->niv_switches;
				usage->ru_nivcsw = task->stats->niv_switches;
				us_to_timeval(
					(info->utime +
					 ps_usage_read(
						 &task->thread->child_utime)) *
						(1000000ULL / HZ),
					&usage->ru_utime);
				us_to_timeval(
					(info->stime +
					 ps_usage_read(
						 &task->thread->child_stime)) *
						(1000000ULL / HZ),
					&usage->ru_stime);
			}
			if (!(options & WNOWAIT)) {
				if (exited)
					detach_exited_child_unsafe(task);
				else
					task->life->stop_report_pending = 0;
			}
			break;
		}
		if (!children) {
			ret = -ECHILD;
			break;
		}
		if (options & WNOHANG)
			break;
		if (ps_interrupting_signals(cur) & ~(1UL << (SIGCHLD - 1))) {
			ret = -EINTR;
			break;
		}
		ps_put_to_wait_queue_unsafe(cur, NULL);
		cur->wait->wait_interruptible = 1;
		spinlock_unlock(&ps_lock, irq);
		task_sched();
		spinlock_lock(&ps_lock, &irq);
	}
	spinlock_unlock(&ps_lock, irq);
	if (task && exited && !(options & WNOWAIT))
		ps_reap_task(task, NULL);
	return ret;
}

/*
 * Public — waitpid
 */

/*
 * Wait for a child to finish, optionally matching a specific pid.
 * Blocks until a child is available:
 *   1. Scan the dying queue; reap and return if a match is found.
 *   2. If no child ready: block on the wait queue.
 *   3. ps_put_to_dying_queue() wakes the parent; re-check on wakeup.
 */
int do_waitpid(unsigned pid, int *status, int options, rusage *rusage)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *task = NULL;
	int ret = -1;
	int irq;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("wait(%d, %x, %x, %x)\n", pid, status, options, rusage);

	for (;;) {
		task = NULL;
		spinlock_lock(&ps_lock, &irq);
		task = find_exited_child_unsafe(cur, pid, 0, 0);
		if (task) {
			ret = ps_pid_visible(task->life->psid);
			if (status)
				*status = task->life->exit_status;
			detach_exited_child_unsafe(task);
			goto done;
		}

		task = find_stopped_child_unsafe(cur, pid, options);
		if (task) {
			ret = ps_pid_visible(task->life->psid);
			if (status)
				*status = W_STOPCODE(task->life->stop_signal);
			task->life->stop_report_pending = 0;
			task = NULL;
			goto done;
		}

		if (pid && !has_child_unsafe(cur, pid)) {
			ret = -ECHILD;
			goto done;
		}

		/* No specific pid requested and no children at all. */
		if (!pid && !has_child_unsafe(cur, 0)) {
			ret = -ECHILD;
			goto done;
		}

		/* Interrupted by a non-SIGCHLD signal: return EINTR. */
		if (ps_interrupting_signals(cur) & ~(1UL << (SIGCHLD - 1))) {
			ret = -EINTR;
			goto done;
		}

		/*
		 * Returns immediately if no child has exited yet.
		 */
		if (options & WNOHANG) {
			ret = 0;
			goto done;
		}

		/* Block until a child exits. ps_put_to_dying_queue() will call
		 * ps_put_to_ready_queue_unsafe(parent) to wake us. */
		ps_put_to_wait_queue_unsafe(cur, NULL);
		cur->wait->wait_interruptible = 1;
		spinlock_unlock(&ps_lock, irq);
		task_sched();
	}

done:
	spinlock_unlock(&ps_lock, irq);

	if (task)
		ps_reap_task(task, rusage);

	if (TEST_LOG(TEST_LOG_INFO))
		klog("wait(%d) returns %d\n", pid, ret);

	return ret;
}

int do_waitpid_pgrp(unsigned pgrp, int *status, int options, rusage *rusage)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *task = NULL;
	int ret = -1;
	int irq;
	int has_group_child = 0;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("wait4(pgrp=%u, %x, %x, %x)\n", pgrp, status, options,
		     rusage);

	for (;;) {
		task = NULL;
		has_group_child = 0;
		spinlock_lock(&ps_lock, &irq);

		task = find_exited_child_unsafe(cur, 0, pgrp, 1);
		if (task) {
			ret = ps_pid_visible(task->life->psid);
			if (status)
				*status = task->life->exit_status;
			detach_exited_child_unsafe(task);
			goto done;
		}

		list_entry *head = &cur->thread->children;
		for (list_entry *entry = head->next; entry != head;
		     entry = entry->next) {
			task_struct *st =
				container_of(entry, task_schedule, child_list)
					->task;

			if (!is_child_unsafe(cur, st) || !st->execution ||
			    st->thread->group_id != pgrp)
				continue;
			has_group_child = 1;
			if (!st->life->stop_report_pending)
				continue;
			if (!(options & WUNTRACED) &&
			    (!st->execution ||
			     st->execution->ptrace_tracer != cur->life->psid))
				continue;
			ret = st->life->psid;
			if (status)
				*status = W_STOPCODE(st->life->stop_signal);
			st->life->stop_report_pending = 0;
			task = NULL;
			goto done;
		}

		task = NULL;
		has_group_child = has_pgrp_child_unsafe(cur, pgrp);

		if (!has_group_child) {
			ret = -ECHILD;
			goto done;
		}

		if (ps_interrupting_signals(cur) & ~(1UL << (SIGCHLD - 1))) {
			ret = -EINTR;
			goto done;
		}

		if (options & WNOHANG) {
			ret = 0;
			goto done;
		}

		ps_put_to_wait_queue_unsafe(cur, NULL);
		cur->wait->wait_interruptible = 1;
		spinlock_unlock(&ps_lock, irq);
		task_sched();
	}

done:
	spinlock_unlock(&ps_lock, irq);

	if (task)
		ps_reap_task(task, rusage);

	if (TEST_LOG(TEST_LOG_INFO))
		klog("wait4(pgrp=%u) returns %d\n", pgrp, ret);

	return ret;
}

int sys_waitpid(unsigned pid, int *status, int options)
{
	int value, ret;
	if (TEST_LOG(TEST_LOG_INFO))
		klog("waitpid(%d, %x, %d)\n", pid, status, options);

	ret = do_waitpid(pid, &value, options, NULL);
	if (ret > 0 && status &&
	    ps_write_process_memory(current, status, &value, sizeof(value)) < 0)
		return -EFAULT;
	return ret;
}

/*
 * Public — misc syscalls
 */

intptr_t sys_getcwd(char *buf, size_t size)
{
	task_struct *cur = CURRENT_TASK();
	const char *cwd = "/";
	size_t length;

	if (!size)
		return -EINVAL;
	if (!buf)
		return -EFAULT;

	if (cur && cur->execution && cur->fs) {
		LOCK_GUARD(&cur->fs->lock);
		if (cur->fs->cwd && cur->fs->cwd[0])
			cwd = cur->fs->cwd;

		length = strlen(cwd) + 1;
		if (length > size)
			return -ERANGE;
		memcpy(buf, cwd, length);
	} else {
		length = strlen(cwd) + 1;
		if (length > size)
			return -ERANGE;
		memcpy(buf, cwd, length);
	}

	if (TEST_LOG(TEST_LOG_INFO))
		klog("getcwd(%s, %u) = %u\n", buf, (unsigned)size,
		     (unsigned)length);

	return (intptr_t)length;
}

/*
 * Public — getrusage
 */

int sys_getrusage(int who, rusage *usage)
{
	task_struct *cur = CURRENT_TASK();

	if (!usage)
		return -EFAULT;
	if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN)
		return -EINVAL;

	memset(usage, 0, sizeof(*usage));

	if (who == RUSAGE_SELF) {
		us_to_timeval(ps_usage_read(&cur->thread->user_tickets) *
				      (1000000ULL / HZ),
			      &usage->ru_utime);
		us_to_timeval(ps_usage_read(&cur->thread->kernel_tickets) *
				      (1000000ULL / HZ),
			      &usage->ru_stime);
		usage->ru_majflt = cur->stats->pf_major;
		usage->ru_minflt = cur->stats->pf_minor;
		usage->ru_nvcsw =
			cur->stats->total_switches - cur->stats->niv_switches;
		usage->ru_nivcsw = cur->stats->niv_switches;
	} else if (who == RUSAGE_CHILDREN) {
		us_to_timeval(ps_usage_read(&cur->thread->child_utime) *
				      (1000000ULL / HZ),
			      &usage->ru_utime);
		us_to_timeval(ps_usage_read(&cur->thread->child_stime) *
				      (1000000ULL / HZ),
			      &usage->ru_stime);
	}

	if (TEST_LOG(TEST_LOG_INFO))
		klog("getrusage(%d) utime=%d stime=%d\n", who,
		     (int)usage->ru_utime.tv_sec, (int)usage->ru_stime.tv_sec);

	return 0;
}

/*
 * Public — shutdown
 */

static void qemu_piix4_poweroff(uint32_t device, uint16_t vendor,
				uint16_t product, void *unused)
{
	unsigned base;
	unsigned short control;
	(void)unused;

	if (vendor != 0x8086 || product != 0x7113)
		return;
	if (!(pci_read_field(device, 0x80, 1) & 1))
		return;
	base = pci_read_field(device, 0x40, 4) & 0xffc0;
	if (!base)
		return;

	/* QEMU PIIX4 uses SLP_TYP 0 and SLP_EN in the PM1 control register. */
	control = port_read_word(base + 4);
	port_write_word(base + 4, (control & ~0x1c00) | 0x2000);
}

void reboot()
{
	/**
	 * This is not a correct reboot but now we just needs a testable procedure.
	 * Force enable interrupt first to make sure fs cache can be flushed.
	 */
	int_intr_enable();
	system_down(0);
	int_intr_disable();
	port_write_byte(0x64, 0xfe);
}

void shutdown()
{
	klog("Shutting down system ...\n");
	int_intr_enable();
	system_down(0);
	int_intr_disable();

	pci_for_each(qemu_piix4_poweroff, PCI_SCAN_ALL, NULL);
	qemu_exit(0x00);

	for (;;)
		HLT();
}
