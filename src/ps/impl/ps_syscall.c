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
#include <mm/mmap.h>
#include <mm/mm.h>
#include <int/int.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/list.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/port.h>
#include <device/time.h>
#include <device/hdd.h>
#include <device/pci.h>
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

static int has_child_unsafe(task_struct *parent, unsigned pid)
{
	struct rb_node *node;

	for (node = rb_first(&control.mgr_queue); node; node = rb_next(node)) {
		task_struct *task =
			(rb_entry(node, task_schedule, mgr_rb)->task);

		if (task->life->ppid != parent->life->psid)
			continue;
		if (pid && task->life->psid != pid)
			continue;
		return 1;
	}

	return 0;
}

static task_struct *find_stopped_child_unsafe(task_struct *parent, unsigned pid,
					      int options)
{
	struct rb_node *node;

	for (node = rb_first(&control.mgr_queue); node; node = rb_next(node)) {
		task_struct *task =
			(rb_entry(node, task_schedule, mgr_rb)->task);

		if (task->life->ppid != parent->life->psid)
			continue;
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
	struct rb_node *node;

	for (node = rb_first(&control.mgr_queue); node; node = rb_next(node)) {
		task_struct *task =
			(rb_entry(node, task_schedule, mgr_rb)->task);

		if (task->life->ppid != parent->life->psid || !task->execution)
			continue;
		if (task->thread->group_id != pgrp)
			continue;
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
	hdd_close();
}

void qemu_exit(unsigned char code)
{
	port_write_byte(0xf4, code);
}

/* Internal: exit with an already-encoded waitpid status word. */
/* Reparent all children of cur to init (pid 1). Must be called without
 * ps_lock held. Living children get a new parent; zombie children also
 * send SIGCHLD to init so it can reap them. */
static void ps_reparent_children(task_struct *cur)
{
	struct rb_node *node;
	int notify_init = 0;
	int irq;

	spinlock_lock(&ps_lock, &irq);
	task_struct *init_task = ps_find_process_unsafe(1);
	if (!init_task || init_task == cur)
		goto out;

	for (node = rb_first(&control.mgr_queue); node; node = rb_next(node)) {
		task_struct *t = (rb_entry(node, task_schedule, mgr_rb)->task);
		if (t->life->ppid != cur->life->psid)
			continue;
		if (t->life->pdeath_signal && t->sighand &&
		    t->sched->status != ps_dying)
			ps_queue_signal_unsafe(t, t->life->pdeath_signal);
		t->life->ppid = init_task->life->psid;
		init_task->life->nchildren++;
		if (t->sched->status == ps_dying)
			notify_init = 1;
	}
	if (notify_init) {
		ps_queue_group_signal_unsafe(init_task, SIGCHLD);
		if (init_task->sched->status == ps_waiting)
			ps_put_to_ready_queue_unsafe(init_task);
	}
out:
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

void ps_reap_dead_threads(void)
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
				break;
			}
		}
		spinlock_unlock(&ps_lock, irq);
		if (!task)
			return;
		ps_reap_group_thread(task);
	}
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
	struct rb_node *node;
	struct rb_node *next;
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
		for (node = rb_first(&control.mgr_queue); node;
		     node = rb_next(node)) {
			task_struct *task =
				(rb_entry(node, task_schedule, mgr_rb)->task);
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
	for (node = rb_first(&control.mgr_queue); node; node = next) {
		task_struct *task =
			(rb_entry(node, task_schedule, mgr_rb)->task);
		next = rb_next(node);

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
		ps_futex_remove_task_locked(task);
		list_remove_entry(&task->sched->ps_list);
		ps_remove_mgr_unsafe(task);
		task->sched->status = ps_dying;
		task->wait->wait_func = NULL;
		list_insert_tail(&reap_list, &task->sched->ps_list);
	}
	spinlock_unlock(&ps_lock, irq);

	while (!list_is_empty(&reap_list)) {
		task_struct *task =
			(container_of(reap_list.next, task_schedule, ps_list)
				 ->task);

		list_remove_entry(&task->sched->ps_list);
		ps_reparent_children(task);
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

	ps_reparent_children(cur);

	if (cur->life->fork_flag & FORK_FLAG_THREAD) {
		int irq;
		spinlock_lock(&ps_lock, &irq);
		ps_remove_mgr_unsafe(cur);

		cur->life->psid = 0xffffffff;
		cur->sched->status = ps_dying;
		list_remove_entry(&cur->sched->ps_list);
		list_insert_tail(&dead_threads, &cur->sched->ps_list);
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
	list_entry *dying_task_entry;
	int ret = -1;
	int irq;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("wait(%d, %x, %x, %x)\n", pid, status, options, rusage);

	for (;;) {
		task = NULL;
		spinlock_lock(&ps_lock, &irq);
		dying_task_entry = cur->life->dying_queue.next;
		while (dying_task_entry != &cur->life->dying_queue) {
			task = (container_of(dying_task_entry, task_schedule,
					     ps_list)
					->task);
			dying_task_entry = dying_task_entry->next;
			if (task->life->ppid != cur->life->psid)
				continue;
			if (task->sched->on_cpu)
				continue;

			if (pid && pid != task->life->psid)
				continue;

			ret = task->life->psid;
			if (status)
				*status = task->life->exit_status;

			list_remove_entry(&task->sched->ps_list);
			ps_remove_mgr_unsafe(task);
			cur->life->nchildren--;
			goto done;
		}

		task = find_stopped_child_unsafe(cur, pid, options);
		if (task) {
			ret = task->life->psid;
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
		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
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
	list_entry *dying_task_entry;
	struct rb_node *node;
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

		dying_task_entry = cur->life->dying_queue.next;
		while (dying_task_entry != &cur->life->dying_queue) {
			task = (container_of(dying_task_entry, task_schedule,
					     ps_list)
					->task);
			dying_task_entry = dying_task_entry->next;
			if (task->life->ppid != cur->life->psid ||
			    !task->execution || task->thread->group_id != pgrp)
				continue;

			has_group_child = 1;
			ret = task->life->psid;
			if (status)
				*status = task->life->exit_status;

			list_remove_entry(&task->sched->ps_list);
			ps_remove_mgr_unsafe(task);
			cur->life->nchildren--;
			goto done;
		}

		for (node = rb_first(&control.mgr_queue); node;
		     node = rb_next(node)) {
			task_struct *st =
				(rb_entry(node, task_schedule, mgr_rb)->task);

			if (st->life->ppid != cur->life->psid ||
			    !st->execution || st->thread->group_id != pgrp)
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

		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
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
	if (TEST_LOG(TEST_LOG_INFO))
		klog("waitpid(%d, %x, %d)\n", pid, status, options);

	return do_waitpid(pid, status, options, NULL);
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
