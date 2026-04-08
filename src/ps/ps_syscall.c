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
#include <hw/time.h>
#include <hw/hdd.h>
#include <config.h>
#include <macro.h>
#include <errno.h>
#include <ext4.h>

#include "ps_internal.h"

/*
 * Static helpers — child reaping
 */

static void ps_reap_task(task_struct *task, rusage *rusage)
{
	task_struct *parent = task->parent;
	unsigned long long child_utime =
		time_now_tickets() - task->stats->start_tickets -
		task->stats->kernel_tickets - task->stats->idle_tickets;

	if (rusage) {
		memset(rusage, 0, sizeof(*rusage));
		rusage->ru_majflt = task->stats->pf_major;
		rusage->ru_minflt = task->stats->pf_minor;
		rusage->ru_nvcsw =
			task->stats->total_switches - task->stats->niv_switches;
		rusage->ru_nivcsw = task->stats->niv_switches;
		ms_to_timeval(task->stats->kernel_tickets * 10,
			      &rusage->ru_stime);
		ms_to_timeval(child_utime * 10, &rusage->ru_utime);
	}

	/* Accumulate child CPU time into parent for cutime/cstime. */
	if (parent && parent->stats) {
		parent->stats->child_utime += child_utime;
		parent->stats->child_stime += task->stats->kernel_tickets;
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
	if (task->user->root_path) {
		name_put(task->user->root_path);
		task->user->root_path = NULL;
	}
	if (task->user->page_dir) {
		vm_free(task->user->page_dir, 1);
		task->user->page_dir = 0;
	}
	if (task->root)
		sb_put(task->root);

	kfree(task->user);
	kfree(task->signal);
	kfree(task->stats);
	vm_free(task, 1);
}

/*
 * Static helpers — system shutdown
 */

static void close_fp_callback(task_struct *task, void *ctx)
{
	int i;
	(void)ctx;
	for (i = 0; i < MAX_FD; i++) {
		if (task->fds == NULL)
			continue;
		if (task->fds[i].used == 0 || task->fds[i].fp == NULL)
			continue;
		fs_put_file(task->fds[i].fp);
	}
}

static void system_down()
{
	klog_close();
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
		if (t->parent != cur)
			continue;
		t->parent = init_task;
		init_task->nchildren++;
		if (t->status == ps_dying)
			notify_init = 1;
	}
	if (notify_init) {
		init_task->signal->sig_pending |= (1UL << (SIGCHLD - 1));
		ps_put_to_ready_queue_unsafe(init_task);
	}
out:
	spinlock_unlock(&ps_lock, irq);
}

void do_exit(unsigned encoded_status)
{
	task_struct *cur = CURRENT_TASK();
	int i;

	cur->exit_status = encoded_status;
	if (TestControl.verbos)
		klog("exit(%s, status=%x)\n", cur->user->command,
		     encoded_status);

	if (cur->fork_flag & FORK_FLAG_VFORK) {
		cond_notify(&cur->vfork_event);
		/* Borrowed page_dir and vm from parent — detach without freeing.
		 * ps_cleanup_all_user_map becomes a no-op with page_dir == 0,
		 * and ps_reap_task skips the vm_free check. */
		cur->user->page_dir = 0;
		cur->user->vm = NULL;
	}

	if (cur->user->vm) {
		/* Flush dirty MAP_SHARED pages while user pages are still mapped. */
		vm_flush_all_dirty(cur->user->vm);
		vm_destroy(cur->user->vm);
		cur->user->vm = 0;
	}

	for (i = 0; i < MAX_FD; i++) {
		if (cur->fds[i].used)
			fs_close(i);
	}
	vm_free(cur->fds, 1);
	cur->fds = NULL;

	ps_cleanup_all_user_map(cur);

	if (cur->psid == 0) {
		printk("fatal error! process 0 exit\n");
		DIE();
	}
	if (cur->psid == 1) {
		if (TestControl.test) {
			unsigned char code =
				(unsigned char)((encoded_status >> 8) & 0xff);
			printf("test mode: init exited with status %u\n", code);
			int_intr_enable();
			system_down();
			int_intr_disable();
			qemu_exit(code);
			for (;;)
				HLT();
		}
		shutdown();
	}

	/* ps_put_to_dying_queue queues SIGCHLD on the parent atomically. */
	ps_reparent_children(cur);
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

	if (TestControl.verbos)
		klog("wait(%d, %x, %x, %x)\n", pid, status, options, rusage);

	for (;;) {
		task = NULL;
		spinlock_lock(&ps_lock, &irq);
		dying_task_entry = control.dying_queue.next;
		while (dying_task_entry != &control.dying_queue) {
			task = container_of(dying_task_entry, task_struct,
					    ps_list);
			dying_task_entry = dying_task_entry->next;
			if (task->parent->psid != cur->psid)
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

		task = NULL;

		if (pid && ps_find_process_unsafe(pid) == NULL) {
			ret = -ECHILD;
			goto done;
		}

		/* No specific pid requested and no children at all. */
		if (!pid && cur->nchildren == 0) {
			ret = -ECHILD;
			goto done;
		}

		/* Interrupted by a non-SIGCHLD signal: return EINTR. */
		if (cur->signal->sig_pending & ~cur->signal->sig_mask &
		    ~(1UL << (SIGCHLD - 1))) {
			ret = -EINTR;
			goto done;
		}

		/*
		 * Returns immediately if no child has exited yet.
		 */
		if (options == WNOHANG) {
			ret = 0;
			goto done;
		}

		/* Block until a child exits. ps_put_to_dying_queue() will call
		 * ps_put_to_ready_queue_unsafe(parent) to wake us. */
		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
		spinlock_unlock(&ps_lock, irq);
		task_sched();
	}

done:
	spinlock_unlock(&ps_lock, irq);

	if (task)
		ps_reap_task(task, rusage);

	if (TestControl.verbos)
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

	if (TestControl.verbos)
		klog("wait4(pgrp=%u, %x, %x, %x)\n", pgrp, status, options,
		     rusage);

	for (;;) {
		task = NULL;
		has_group_child = 0;
		spinlock_lock(&ps_lock, &irq);

		dying_task_entry = control.dying_queue.next;
		while (dying_task_entry != &control.dying_queue) {
			task = container_of(dying_task_entry, task_struct,
					    ps_list);
			dying_task_entry = dying_task_entry->next;
			if (task->parent->psid != cur->psid || !task->user ||
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

		task = NULL;
		node = rb_first(&control.mgr_queue);
		while (node) {
			task_struct *t = rb_entry(node, task_struct, mgr_rb);
			node = rb_next(node);
			if (t->parent->psid == cur->psid && t->user &&
			    t->user->group_id == pgrp) {
				has_group_child = 1;
				break;
			}
		}

		if (!has_group_child) {
			ret = -ECHILD;
			goto done;
		}

		if (cur->signal->sig_pending & ~cur->signal->sig_mask &
		    ~(1UL << (SIGCHLD - 1))) {
			ret = -EINTR;
			goto done;
		}

		if (options == WNOHANG) {
			ret = 0;
			goto done;
		}

		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);
		spinlock_unlock(&ps_lock, irq);
		task_sched();
	}

done:
	spinlock_unlock(&ps_lock, irq);

	if (task)
		ps_reap_task(task, rusage);

	if (TestControl.verbos)
		klog("wait4(pgrp=%u) returns %d\n", pgrp, ret);

	return ret;
}

int sys_waitpid(unsigned pid, int *status, int options)
{
	if (TestControl.verbos)
		klog("waitpid(%d, %x, %d)\n", pid, status, options);

	return do_waitpid(pid, status, options, NULL);
}

/*
 * Public — misc syscalls
 */

char *sys_getcwd(char *buf, unsigned size)
{
	task_struct *cur = CURRENT_TASK();
	const char *cwd = "/";

	if (cur && cur->user && cur->user->cwd && cur->user->cwd[0])
		cwd = cur->user->cwd;

	strcpy(buf, cwd);

	if (TestControl.verbos)
		klog("getcwd(%s, %d) = %s\n", buf, size, buf);

	return buf;
}

/*
 * Public — getrusage
 */

int sys_getrusage(int who, rusage *usage)
{
	task_struct *cur = CURRENT_TASK();

	if (!usage)
		return -1;

	memset(usage, 0, sizeof(*usage));

	if (who == RUSAGE_SELF) {
		ms_to_timeval((time_now_tickets() - cur->stats->start_tickets -
			       cur->stats->kernel_tickets -
			       cur->stats->idle_tickets) *
				      10,
			      &usage->ru_utime);
		ms_to_timeval(cur->stats->kernel_tickets * 10,
			      &usage->ru_stime);
		usage->ru_majflt = cur->stats->pf_major;
		usage->ru_minflt = cur->stats->pf_minor;
		usage->ru_nvcsw =
			cur->stats->total_switches - cur->stats->niv_switches;
		usage->ru_nivcsw = cur->stats->niv_switches;
	} else if (who == RUSAGE_CHILDREN) {
		ms_to_timeval(cur->stats->child_utime * 10, &usage->ru_utime);
		ms_to_timeval(cur->stats->child_stime * 10, &usage->ru_stime);
	}

	if (TestControl.verbos)
		klog("getrusage(%d) utime=%d stime=%d\n", who,
		     (int)usage->ru_utime.tv_sec, (int)usage->ru_stime.tv_sec);

	return 0;
}

/*
 * Public — shutdown
 */

void reboot()
{
	/**
	 * This is not a correct reboot but now we just needs a testable procedure.
	 * Force enable interrupt first to make sure fs cache can be flushed.
	 */
	int_intr_enable();
	system_down();
	int_intr_disable();
	port_write_byte(0x64, 0xfe);
}

void shutdown()
{
	/**
	 * This is not a correct shutdown but now we just needs a testable procedure.
	 * Force enable interrupt first to make sure fs cache can be flushed.
	 */
	const char s[] = "Shutdown";
	const char *p;
	printf("Shutting down system ...\n");
	int_intr_enable();
	system_down();
	int_intr_disable();

	printf("Power off...\n");
	for (p = s; *p != '\0'; p++)
		port_write_byte(0x8900, *p);

	qemu_exit(0x00);

	printf("still running...\n");
	for (;;)
		HLT();
}
