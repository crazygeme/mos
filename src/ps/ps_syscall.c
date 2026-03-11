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

#include <ps.h>
#include <cpu.h>
#include <mount.h>
#include <mmap.h>
#include <phymm.h>
#include <time.h>
#include <lock.h>
#include <mm.h>
#include <klib.h>
#include <config.h>
#include <int.h>
#include <list.h>
#include <fs.h>
#include <macro.h>
#include <port.h>
#include <ptrace.h>
#include <block.h>
#include "ps_internal.h"

extern void ret_from_syscall();
extern short pgc_entry_count[1024];

/* -------------------------------------------------------------------------
 * Static helpers — file-descriptor duplication
 * ------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Static helpers — COW user address-space duplication
 * ------------------------------------------------------------------------- */

/* Map one user page from the parent into the child using COW:
 * - mark parent's page read-only so a write triggers a fault
 * - share the physical page with the child (increment refcount) */
static void ps_dup_user_page(unsigned vir, task_struct *task, unsigned flag)
{
	unsigned int *new_ps = (unsigned int *)task->user.page_dir;
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

	task->user.page_dir = vm_alloc(1);
	vm_dup(cur->user.vm, task->user.vm);
	new_ps = (unsigned int *)task->user.page_dir;
	memset((char *)new_ps, 0, PAGE_SIZE);
	ps_enum_user_map(cur, ps_enum_for_dup, task);
}

/* -------------------------------------------------------------------------
 * Static helpers — child reaping
 * ------------------------------------------------------------------------- */

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
	if (task->command) {
		vm_free(task->command, 1);
		task->command = NULL;
	}
	if (task->cwd) {
		name_put(task->cwd);
		task->cwd = NULL;
	}
	if (task->user.page_dir) {
		vm_free(task->user.page_dir, 1);
		task->user.page_dir = 0;
	}
	if (task->root)
		sb_put(task->root);
	vm_free(task, 1);
}

/* -------------------------------------------------------------------------
 * Static helpers — system shutdown
 * ------------------------------------------------------------------------- */

static void close_fp_callback(task_struct *task)
{
	int i;
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
	ps_enum_all(close_fp_callback);
	ext4_umount("/");
	block_close();
}

/* -------------------------------------------------------------------------
 * Public — fork
 * ------------------------------------------------------------------------- */

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
	task->tss.eip = (unsigned)ret_from_syscall;
	task_intr_frame->eax = 0;

	task->parent = cur;
	task->fds = vm_alloc(1);
	list_init(&task->ps_list);
	task->user.vm = vm_create();
	task->command = vm_alloc(1);
	task->umask = cur->umask;
	task->priority = cur->priority;
	strcpy(task->command, cur->command);
	task->cwd = name_get();
	task->fork_flag = flag;
	task->root = cur->root;
	sb_get(task->root);
	memset(task->cwd, 0, MAX_PATH);
	strcpy(task->cwd, cur->cwd);

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
		klog("%d: fork()\n", CURRENT_TASK()->psid);
	return do_fork(0);
}

int sys_vfork()
{
	if (TestControl.verbos)
		klog("%d: vfork()\n", CURRENT_TASK()->psid);
	return do_fork(FORK_FLAG_VFORK);
}

/* -------------------------------------------------------------------------
 * Public — exit
 * ------------------------------------------------------------------------- */

int sys_exit(unsigned status)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *parent = NULL;
	int i;

	cur->exit_status = status;
	if (TestControl.verbos)
		klog("%d: exit(%d)\n", cur->psid, status);

	if (cur->fork_flag & FORK_FLAG_VFORK)
		cond_notify(&cur->vfork_event);

	if (cur->user.vm) {
		vm_destroy(cur->user.vm);
		cur->user.vm = 0;
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
	ps_put_to_dying_queue(cur);

	task_sched();
	return 0;
}

/* -------------------------------------------------------------------------
 * Public — waitpid
 * ------------------------------------------------------------------------- */

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
	struct rb_node *task_entry;

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
				klog("%d: wait(%d) = %d\n", cur->psid, pid,
				     ret);
			return ret;
		}

		task_entry = rb_first(&control.mgr_queue);
		while (task_entry) {
			task_struct *task =
				rb_entry(task_entry, task_struct, mgr_rb);
			task_entry = rb_next(task_entry);
			if (task->parent->psid != cur->psid)
				continue;

			if (!(task->ptrace & PT_TRACED) ||
			    !(task->ptrace & PT_STOPPED))
				continue;

			if (task->ptrace & PT_STOP_REPORTED)
				continue;

			if (pid && pid != task->psid)
				continue;

			int ret = task->psid;
			if (status)
				*status = (SIGTRAP << 8) | 0x7f;

			task->ptrace |= PT_STOP_REPORTED;
			spinlock_unlock(&ps_lock);
			if (TestControl.verbos)
				klog("%d: wait(%d) = %d\n", cur->psid, pid,
				     ret);
			return ret;
		}

		ps_put_to_wait_queue_unsafe(cur, NULL, __func__);

		spinlock_unlock(&ps_lock);
	}
}

int sys_waitpid(unsigned pid, int *status, int options)
{
	return do_waitpid(pid, status, options, NULL);
}

/* -------------------------------------------------------------------------
 * Public — misc syscalls
 * ------------------------------------------------------------------------- */

char *sys_getcwd(char *buf, unsigned size)
{
	task_struct *cur = CURRENT_TASK();
	strcpy(buf, cur->cwd[0] ? cur->cwd : "/");
	return buf;
}

/* -------------------------------------------------------------------------
 * Public — shutdown
 * ------------------------------------------------------------------------- */

void reboot()
{
	system_down();
	DISABLE_INTR();
	port_write_byte(0x64, 0xfe);
}

void shutdown()
{
	const char s[] = "Shutdown";
	const char *p;
	printf("Shutting down system ...\n");
	system_down();
	DISABLE_INTR();

	printf("Power off...\n");
	for (p = s; *p != '\0'; p++)
		port_write_byte(0x8900, *p);

	port_write_byte(0xf4, 0x00);

	printf("still running...\n");
	for (;;)
		HLT();
}
