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
	while (__atomic_load_n(&task->enumeration_refs, __ATOMIC_ACQUIRE))
		time_wait(1);
	unsigned long long child_utime =
		ps_usage_read(&task->usage->user_tickets) +
		ps_usage_read(&task->usage->child_utime);
	unsigned long long child_stime =
		ps_usage_read(&task->usage->kernel_tickets) +
		ps_usage_read(&task->usage->child_stime);

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
	if (!(task->fork_flag & FORK_FLAG_THREAD)) {
		int irq;
		spinlock_lock(&ps_lock, &irq);
		task_struct *parent = ps_find_process_unsafe(task->ppid);
		if (parent && parent->usage) {
			ps_usage_add_local(&parent->usage->child_utime,
					   child_utime);
			ps_usage_add_local(&parent->usage->child_stime,
					   child_stime);
		}
		spinlock_unlock(&ps_lock, irq);
	}

	if (task->user->command) {
		vm_free(task->user->command, 1);
		task->user->command = NULL;
		task->user->cmd_len = 0;
	}
	if (task->user->environment) {
		vm_free(task->user->environment, 1);
		task->user->environment = NULL;
		task->user->env_len = 0;
	}
	if (task->user->cwd) {
		name_put(task->user->cwd);
		task->user->cwd = NULL;
	}
	if (task->user->executable) {
		fs_put_file(task->user->executable);
		task->user->executable = NULL;
	}
	if (task->user->root_path) {
		name_put(task->user->root_path);
		task->user->root_path = NULL;
	}
	if (task->user->vm) {
		vm_put(task->user->vm);
		task->user->vm = NULL;
	}
	if (task->root)
		sb_put(task->root);

	kfree(task->user);
	kfree(task->signal);
	ps_usage_put(task);
	kfree(task->stats);
	ps_put_fds(task);
	kfree(task->io_bitmap);
	vm_free((vaddr_t)task, KERNEL_TASK_SIZE);
}

static int has_child_unsafe(task_struct *parent, unsigned pid)
{
	struct rb_node *node;

	for (node = rb_first(&control.mgr_queue); node; node = rb_next(node)) {
		task_struct *task = rb_entry(node, task_struct, mgr_rb);

		if (task->ppid != parent->psid)
			continue;
		if (pid && task->psid != pid)
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
		task_struct *task = rb_entry(node, task_struct, mgr_rb);

		if (task->ppid != parent->psid)
			continue;
		if (pid && task->psid != pid)
			continue;
		if (!task->stop_report_pending)
			continue;
		if (!(options & WUNTRACED) &&
		    (!task->user || task->user->ptrace_tracer != parent->psid))
			continue;
		return task;
	}

	return NULL;
}

static int has_pgrp_child_unsafe(task_struct *parent, unsigned pgrp)
{
	struct rb_node *node;

	for (node = rb_first(&control.mgr_queue); node; node = rb_next(node)) {
		task_struct *task = rb_entry(node, task_struct, mgr_rb);

		if (task->ppid != parent->psid || !task->user)
			continue;
		if (task->user->group_id != pgrp)
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
		task_struct *t = rb_entry(node, task_struct, mgr_rb);
		if (t->ppid != cur->psid)
			continue;
		if (t->pdeath_signal && t->signal && t->status != ps_dying)
			ps_queue_signal_unsafe(t, t->pdeath_signal);
		t->ppid = init_task->psid;
		init_task->nchildren++;
		if (t->status == ps_dying)
			notify_init = 1;
	}
	if (notify_init) {
		init_task->signal->sig_pending |= (1UL << (SIGCHLD - 1));
		if (init_task->status == ps_waiting)
			ps_put_to_ready_queue_unsafe(init_task);
	}
out:
	spinlock_unlock(&ps_lock, irq);
}

static void ps_cancel_io_wait(task_struct *task)
{
	/* Detach I/O waiters before closing files or freeing the kernel stack. */
	if (task->cancel_io_wait) {
		void (*cancel)(void *) = task->cancel_io_wait;
		void *wait = task->io_wait;
		task->cancel_io_wait = NULL;
		task->io_wait = NULL;
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
				container_of(node, task_struct, ps_list);
			if (!candidate->on_cpu &&
			    !candidate->enumeration_refs) {
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
	list_remove_entry(&current->ps_list);
	list_init(&current->ps_list);
	current->status = ps_stopped;
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
	ps_timer_discard_group(leader->tgid);

	list_init(&reap_list);

	/* Stop remote users before touching their VM, descriptors or stack.
	 * Release ps_lock before waiting so remote timer entries can stop tasks. */
	for (;;) {
		int active = 0;
		spinlock_lock(&ps_lock, &irq);
		for (node = rb_first(&control.mgr_queue); node;
		     node = rb_next(node)) {
			task_struct *task = rb_entry(node, task_struct, mgr_rb);
			if (task != leader && task->tgid == leader->tgid &&
			    task->type == ps_user) {
				task->terminate_requested = 1;
				active |= task->on_cpu != 0 ||
					  task->vm_lock_depth != 0;
			}
		}
		spinlock_unlock(&ps_lock, irq);
		if (!active)
			break;
		time_wait(1);
	}

	spinlock_lock(&ps_lock, &irq);
	for (node = rb_first(&control.mgr_queue); node; node = next) {
		task_struct *task = rb_entry(node, task_struct, mgr_rb);
		next = rb_next(node);

		if (task == leader)
			continue;
		if (task->type != ps_user || !task->signal)
			continue;
		if (task->tgid != leader->tgid)
			continue;

		/*
		 * exit_group() must remove sibling threads from every scheduler data
		 * structure before the leader destroys shared VM/file-table state.
		 */
		timer_disarm_unsafe(task);
		ps_futex_remove_task_locked(task);
		list_remove_entry(&task->ps_list);
		ps_remove_mgr_unsafe(task);
		task->status = ps_dying;
		task->wait_func = NULL;
		list_insert_tail(&reap_list, &task->ps_list);
	}
	spinlock_unlock(&ps_lock, irq);

	while (!list_is_empty(&reap_list)) {
		task_struct *task =
			container_of(reap_list.next, task_struct, ps_list);

		list_remove_entry(&task->ps_list);
		ps_reparent_children(task);
		if (task->fork_flag & FORK_FLAG_THREAD) {
			ps_reap_group_thread(task);
		} else {
			/* Retain the process leader as the parent's waitable zombie. */
			ps_cancel_io_wait(task);
			ps_clear_child_tid(task);
			ps_release_robust_list(task);
			ps_put_fds(task);
			if (task->user->executable) {
				fs_put_file(task->user->executable);
				task->user->executable = NULL;
			}
			task->exit_status = encoded_status;
			ps_put_to_dying_queue(task);
		}
	}
}

void do_group_exit(unsigned encoded_status)
{
	fs_posix_lock_release(NULL, CURRENT_TASK()->tgid);
	ps_kill_thread_group(CURRENT_TASK(), encoded_status);
	do_exit(encoded_status);
}

void do_exit(unsigned encoded_status)
{
	task_struct *cur = CURRENT_TASK();

	ps_ptrace_stop_exit(encoded_status);
	cur->exit_status = encoded_status;
	if (!(cur->fork_flag & FORK_FLAG_THREAD)) {
		fs_posix_lock_release(NULL, cur->tgid);
		ps_timer_discard_group(cur->tgid);
	}
	if (TEST_LOG(TEST_LOG_INFO))
		klog("exit(%s, status=%x)\n", cur->user->command,
		     encoded_status);

	if (cur->fork_flag & FORK_FLAG_VFORK) {
		cond_notify(&cur->vfork_event);
		vm_put(cur->user->vm);
		cur->user->vm = NULL;
	}

	ps_cancel_io_wait(cur);
	ps_clear_child_tid(cur);
	ps_release_robust_list(cur);

	if (cur->user->vm) {
		/* Flush dirty MAP_SHARED pages while user pages are still mapped. */
		vm_flush_all_dirty(cur->user->vm);
	}

	ps_put_fds(cur);
	if (cur->user->executable) {
		fs_put_file(cur->user->executable);
		cur->user->executable = NULL;
	}

	if (cur->psid == 0) {
		printk("fatal error! process 0 exit\n");
		DIE();
	}
	if (cur->psid == 1) {
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

	if (cur->fork_flag & FORK_FLAG_THREAD) {
		int irq;
		spinlock_lock(&ps_lock, &irq);
		ps_remove_mgr_unsafe(cur);

		cur->psid = 0xffffffff;
		cur->tgid = 0xffffffff;
		cur->status = ps_dying;
		list_remove_entry(&cur->ps_list);
		list_insert_tail(&dead_threads, &cur->ps_list);
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
		dying_task_entry = cur->dying_queue.next;
		while (dying_task_entry != &cur->dying_queue) {
			task = container_of(dying_task_entry, task_struct,
					    ps_list);
			dying_task_entry = dying_task_entry->next;
			if (task->ppid != cur->psid)
				continue;
			if (task->on_cpu)
				continue;

			if (pid && pid != task->psid)
				continue;

			ret = task->psid;
			if (status)
				*status = task->exit_status;

			list_remove_entry(&task->ps_list);
			ps_remove_mgr_unsafe(task);
			cur->nchildren--;
			goto done;
		}

		task = find_stopped_child_unsafe(cur, pid, options);
		if (task) {
			ret = task->psid;
			if (status)
				*status = W_STOPCODE(task->stop_signal);
			task->stop_report_pending = 0;
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
		cur->wait_interruptible = 1;
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

		dying_task_entry = cur->dying_queue.next;
		while (dying_task_entry != &cur->dying_queue) {
			task = container_of(dying_task_entry, task_struct,
					    ps_list);
			dying_task_entry = dying_task_entry->next;
			if (task->ppid != cur->psid || !task->user ||
			    task->user->group_id != pgrp)
				continue;

			has_group_child = 1;
			ret = task->psid;
			if (status)
				*status = task->exit_status;

			list_remove_entry(&task->ps_list);
			ps_remove_mgr_unsafe(task);
			cur->nchildren--;
			goto done;
		}

		for (node = rb_first(&control.mgr_queue); node;
		     node = rb_next(node)) {
			task_struct *st = rb_entry(node, task_struct, mgr_rb);

			if (st->ppid != cur->psid || !st->user ||
			    st->user->group_id != pgrp)
				continue;
			has_group_child = 1;
			if (!st->stop_report_pending)
				continue;
			if (!(options & WUNTRACED) &&
			    (!st->user || st->user->ptrace_tracer != cur->psid))
				continue;
			ret = st->psid;
			if (status)
				*status = W_STOPCODE(st->stop_signal);
			st->stop_report_pending = 0;
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
		cur->wait_interruptible = 1;
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

	if (cur && cur->user && cur->user->cwd && cur->user->cwd[0])
		cwd = cur->user->cwd;

	length = strlen(cwd) + 1;
	if (length > size)
		return -ERANGE;
	memcpy(buf, cwd, length);

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
		us_to_timeval(ps_usage_read(&cur->usage->user_tickets) *
				      (1000000ULL / HZ),
			      &usage->ru_utime);
		us_to_timeval(ps_usage_read(&cur->usage->kernel_tickets) *
				      (1000000ULL / HZ),
			      &usage->ru_stime);
		usage->ru_majflt = cur->stats->pf_major;
		usage->ru_minflt = cur->stats->pf_minor;
		usage->ru_nvcsw =
			cur->stats->total_switches - cur->stats->niv_switches;
		usage->ru_nivcsw = cur->stats->niv_switches;
	} else if (who == RUSAGE_CHILDREN) {
		us_to_timeval(ps_usage_read(&cur->usage->child_utime) *
				      (1000000ULL / HZ),
			      &usage->ru_utime);
		us_to_timeval(ps_usage_read(&cur->usage->child_stime) *
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
