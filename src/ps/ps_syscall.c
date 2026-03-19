/*
 * ps_syscall.c — process syscalls: fork, exit, wait, getcwd; and shutdown.
 *
 * Owns:
 *   - COW fork (do_fork, sys_fork, sys_vfork)
 *   - Process exit (sys_exit)
 *   - Child reaping (do_waitpid, sys_waitpid)
 *   - sys_getcwd
 *   - System shutdown (reboot, shutdown)
 */

#include <ps/ps.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/mm.h>
#include <int/int.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/list.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/port.h>
#include <hw/time.h>
#include <hw/cpu.h>
#include <hw/hdd.h>
#include <config.h>
#include <macro.h>
#include <errno.h>
#include <ext4.h>

#include "ps_internal.h"

extern void ret_from_syscall();
extern void ret_from_fork();
extern short pgc_entry_count[1024];

/*
 * Static helpers — file-descriptor duplication
 */

static void ps_dup_fds(task_struct *cur, task_struct *task)
{
	int i;
	memset(task->fds, 0, PAGE_SIZE);
	mutex_lock(&cur->fd_lock);
	for (i = 0; i < MAX_FD; i++) {
		if (!cur->fds[i].used)
			continue;
		task->fds[i] = cur->fds[i];
		fs_get_file(cur->fds[i].fp);
	}
	mutex_unlock(&cur->fd_lock);
}

/*
 * Static helpers — COW user address-space duplication
 */

/* Map one user page from the parent into the child using COW:
 * - mark parent's page read-only so a write triggers a fault
 * - share the physical page with the child (increment refcount) */
static void ps_dup_user_page(unsigned vir, task_struct *task, unsigned flag)
{
	unsigned int *new_ps = (unsigned int *)task->user->page_dir;
	unsigned target_page_idx = mm_get_attached_page_index(vir);
	unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
	unsigned int page_table_offset = ADDR_TO_PET_OFFSET(vir);
	unsigned int *cur_page_dir = mm_get_pagedir();
	unsigned int *page_table;
	unsigned int *cur_page_table;
	unsigned int phy;
	int idx;

	if (!(new_ps[page_dir_offset] & PAGE_SIZE_MASK)) {
		new_ps[page_dir_offset] = mm_alloc_page_table() - KERNEL_OFFSET;
		new_ps[page_dir_offset] |= flag;
	}

	page_table = (new_ps[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET;
	cur_page_table = (cur_page_dir[page_dir_offset] & PAGE_SIZE_MASK) +
			 KERNEL_OFFSET;

	cur_page_table[page_table_offset] &= ~PAGE_ENTRY_WRITABLE;

	phy = page_table[page_table_offset] & PAGE_SIZE_MASK;
	if (!phy) {
		idx = (PAGE_TABLE_CACHE_END - (unsigned)page_table) /
			      PAGE_SIZE -
		      1;
		pgc_entry_count[idx]++;
	}

	page_table[page_table_offset] = cur_page_table[page_table_offset];
	phymm_reference_page(target_page_idx);
}

static void ps_enum_for_dup(void *aux, unsigned vir, unsigned phy)
{
	ps_dup_user_page(vir, (task_struct *)aux, PAGE_ENTRY_USER_DATA);
}

static void ps_dup_user_maps(task_struct *cur, task_struct *task)
{
	unsigned int *new_ps;

	vm_dup(cur->user->vm, task->user->vm);
	new_ps = (unsigned int *)task->user->page_dir;
	memset((char *)new_ps, 0, PAGE_SIZE);
	ps_enum_user_map(cur, ps_enum_for_dup, task);
}

/*
 * Static helpers — child reaping
 */

static void ps_reap_task(task_struct *task, rusage *rusage)
{
	if (rusage) {
		memset(rusage, 0, sizeof(*rusage));
		rusage->ru_majflt = task->pf_major;
		rusage->ru_minflt = task->pf_minor;
		rusage->ru_nivcsw = task->niv_switches;
		ms_to_timeval(task->kernel_tickets * 10, &rusage->ru_stime);
		ms_to_timeval(task->user_tickets * 10, &rusage->ru_utime);
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
	if (task->user->page_dir) {
		vm_free(task->user->page_dir, 1);
		task->user->page_dir = 0;
	}
	if (task->root)
		sb_put(task->root);

	kfree(task->user);
	kfree(task->signal);
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

/*
 * Public — fork
 */

/* Core fork implementation. Creates a copy of the current task with COW
 * user address space. If FORK_FLAG_VFORK, the parent blocks until the
 * child calls exec or exit. */
int do_fork(unsigned flag)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *task = vm_alloc(KERNEL_TASK_SIZE);
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	intr_frame *task_intr_frame =
		(intr_frame *)((char *)task + PAGE_SIZE - sizeof(intr_frame));

	*task = *cur;
	*task_intr_frame = *cur_intr_frame;

	if (cur->remain_ticks > DEFAULT_TASK_TIME_SLICE / 2)
		cur->remain_ticks = task->remain_ticks = cur->remain_ticks / 2;
	else
		task->remain_ticks = cur->remain_ticks;
	task->timeout = cur->timeout;
	task->psid = ps_id_gen();
	mutex_init(&task->fd_lock);

	task->tss.fs = task->tss.gs = task->tss.ds = task->tss.es =
		task->tss.ss = KERNEL_DATA_SELECTOR;
	task->tss.cs = KERNEL_CODE_SELECTOR;
	task->tss.eax = 0;
	task->tss.ebp = (char *)task_intr_frame;
	task->tss.esp = (char *)task_intr_frame;
	task->tss.esp0 = (unsigned)task + PAGE_SIZE;
	task->tss.eip = (unsigned)ret_from_fork;
	task_intr_frame->eax = 0;

	task->parent = cur;
	task->fds = vm_alloc(1);
	list_init(&task->ps_list);
	task->user = zalloc(sizeof(user_enviroment));
	task->user->vm = vm_create();
	task->user->command = vm_alloc(1);
	task->user->cmd_len = cur->user->cmd_len;
	task->user->environment = vm_alloc(1);
	task->user->env_len = cur->user->env_len;
	memcpy(task->user->command, cur->user->command, cur->user->cmd_len);
	memcpy(task->user->environment, cur->user->environment,
	       cur->user->env_len);
	task->user->cwd = name_get();
	strcpy(task->user->cwd, cur->user->cwd);
	task->user->heap_top = cur->user->heap_top;
	task->user->page_dir = vm_alloc(1);
	task->user->group_id = cur->user->group_id;
	task->user->session_id = cur->user->session_id;

	task->signal = zalloc(sizeof(signal_context));
	memcpy(task->signal, cur->signal, sizeof(signal_context));
	// child starts with no signal pending
	task->signal->sig_pending = 0;

	task->umask = cur->umask;
	task->priority = cur->priority;
	task->type = cur->type;

	task->fork_flag = flag;
	task->root = cur->root;
	sb_get(task->root);

	ps_dup_fds(cur, task);
	ps_dup_user_maps(cur, task);
	spinlock_lock(&ps_lock);
	ps_put_to_ready_queue_unsafe(task);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock);

	if (flag & FORK_FLAG_VFORK) {
		cond_init(&task->vfork_event, 1);
		cond_wait(&task->vfork_event);
	}

	cur_intr_frame->eax = task->psid;
	return task->psid;
}

int sys_fork()
{
	if (TestControl.verbos)
		klog("fork()\n");
	return do_fork(0);
}

int sys_vfork()
{
	if (TestControl.verbos)
		klog("vfork()\n");
	return do_fork(FORK_FLAG_VFORK);
}

/*
 * Public — exit
 */

int sys_exit(unsigned status)
{
	task_struct *cur = CURRENT_TASK();
	int i;

	cur->exit_status = status;
	if (TestControl.verbos)
		klog("exit(%s, %d)\n", cur->user->command, status);

	if (cur->fork_flag & FORK_FLAG_VFORK)
		cond_notify(&cur->vfork_event);

	if (cur->user->vm) {
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
	if (cur->psid == 1)
		shutdown();

	/* FIXME: reparent orphaned children to cur->parent. */
	/* ps_put_to_dying_queue queues SIGCHLD on the parent atomically. */
	ps_put_to_dying_queue(cur);

	task_sched();
	return 0;
}

/*
 * Public — waitpid
 */

/*
 * Wait for a child to finish, optionally matching a specific pid.
 * Loops until a child is available:
 *   1. Yield the CPU.
 *   2. Scan the dying queue; reap and return if a match is found.
 *   3. Scan mgr_queue for a ptrace-stopped child; report and return if found.
 *   4. No child ready: block on the wait queue and retry.
 */
int do_waitpid(unsigned pid, int *status, int options, rusage *rusage)
{
	task_struct *cur = CURRENT_TASK();
	list_entry *dying_task_entry;

	for (;;) {
		task_sched();

		spinlock_lock(&ps_lock);
		dying_task_entry = control.dying_queue.next;
		while (dying_task_entry != &control.dying_queue) {
			task_struct *task = container_of(dying_task_entry,
							 task_struct, ps_list);
			dying_task_entry = dying_task_entry->next;
			if (task->parent->psid != cur->psid)
				continue;

			if (pid && pid != task->psid)
				continue;

			int ret = task->psid;
			if (status)
				*status = task->exit_status;

			list_remove_entry(&task->ps_list);
			ps_remove_mgr_unsafe(task);
			spinlock_unlock(&ps_lock);
			ps_reap_task(task, rusage);
			if (TestControl.verbos)
				klog("wait(%d) = %d\n", pid, ret);
			return ret;
		}

		/* Interrupted by a non-SIGCHLD signal: return EINTR. */
		if (cur->signal->sig_pending & ~cur->signal->sig_mask &
		    ~(1UL << (SIGCHLD - 1))) {
			spinlock_unlock(&ps_lock);
			return -EINTR;
		}

		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);

		spinlock_unlock(&ps_lock);
	}
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
	strcpy(buf, cur->user->cwd[0] ? cur->user->cwd : "/");

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
		ms_to_timeval(cur->user_tickets * 10, &usage->ru_utime);
		ms_to_timeval(cur->kernel_tickets * 10, &usage->ru_stime);
		usage->ru_majflt = cur->pf_major;
		usage->ru_minflt = cur->pf_minor;
		usage->ru_nivcsw = cur->niv_switches;
	}
	/* RUSAGE_CHILDREN: no accumulated child stats yet, return zeros */

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

	port_write_byte(0xf4, 0x00);

	printf("still running...\n");
	for (;;)
		HLT();
}
