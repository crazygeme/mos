/*
 * ps_fork.c — task creation: ps_create, fork, vfork.
 *
 * Owns:
 *   - Kernel task entry point and creation (ps_run, ps_create)
 *   - COW fork (do_fork, sys_fork)
 *   - Real vfork (do_vfork, sys_vfork)
 */

#include <lib/klib.h>
#include <ps/ps.h>
#include <ps/clone.h>
#include <int/int.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/mm.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <errno.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <config.h>
#include <macro.h>
#include <mm/mmu.h>

#include "ps_internal.h"
#include <ps/usage.h>
#include <ps/smp.h>

static int ps_init_fds(task_struct *task);
static void ps_release_files(ref_count_t *ref);

extern void ret_from_fork();
extern short pgc_entry_count[PAGE_TABLE_CACHE_PAGES];

/*
 * Kernel task entry point
 */

/* Every kernel task begins here. Enables interrupts, runs the task function,
 * then moves the task to the dying queue and yields. */
static void ps_run()
{
	task_struct *task = CURRENT_TASK();
	process_fn fn;

	int_intr_enable();
	_ps_enabled = 1;
	task->sched->status = ps_running;
	fn = task->life->fn;
	if (fn)
		fn(task->life->param);

	ps_put_to_dying_queue(task);
	ps_remove_mgr(task);
	task_sched();
}

/*
 * Public — kernel task creation
 */

/* Allocate and initialise a new kernel task. Returns the new psid, or
 * 0xffffffff on failure. The task is immediately placed in the ready queue. */
unsigned _ps_create(process_fn fn, const char *name, void *param,
		    ps_priority priority, ps_type type)
{
	uintptr_t stack_bottom;
	task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);
	int irq;

	if (priority >= PS_PRIORITY_MAX) {
		vm_free((vaddr_t)task, KERNEL_TASK_SIZE);
		return 0xffffffff;
	}

	if (!task)
		return -ENOMEM;
	memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);
	if (ps_init_fds(task) != 0) {
		vm_free(task, KERNEL_TASK_SIZE);
		return -ENOMEM;
	}

	if (ps_init_resources(task)) {
		ps_free_task(task);
		return -ENOMEM;
	}
	task->memory = vm_create();
	if (task->memory)
		task->memory->page_dir = vm_alloc(1);
	if (!task->memory || !task->memory->page_dir) {
		ps_free_task(task);
		return -ENOMEM;
	}
	sprintf(task->memory->command, "sys-%s", name);
	task->memory->cmd_len = strlen(task->memory->command) + 1;
	*((char *)task->memory->environment) = '\0';
	task->memory->env_len = 0;
	mm_init_process_page_dir(task->memory->page_dir);
	memset(task->fs->cwd, 0, MAX_PATH);
	strcpy(task->fs->root_path, "/");
	/* Default rlimits: RLIM_INFINITY for all, except known constraints. */
	for (int i = 0; i < RLIM_NLIMITS; i++) {
		task->thread->rlimits[i].rlim_cur = RLIM_INFINITY;
		task->thread->rlimits[i].rlim_max = RLIM_INFINITY;
	}
	task->thread->rlimits[3].rlim_cur =
		USER_STACK_PAGES * PAGE_SIZE; /* RLIMIT_STACK */
	task->thread->rlimits[3].rlim_max = USER_STACK_PAGES * PAGE_SIZE;
	task->thread->rlimits[4].rlim_cur = 0; /* RLIMIT_CORE: no core dumps */
	task->thread->rlimits[4].rlim_max = 0;
	/* RLIMIT_NOFILE: match Linux default of 1024/1024.  Services that do
	 * malloc(rl_cur * ...) won't OOM, and glibc sizes its fd table to 1024
	 * which matches xinetd's max_descriptors after its setrlimit call. */
	task->thread->rlimits[7].rlim_cur = 1024;
	task->thread->rlimits[7].rlim_max = 1024;

	stack_bottom = (uintptr_t)task + KERNEL_TASK_BYTES;
	list_init(&task->sched->ps_list);
	list_init(&task->life->dying_queue);
	list_init(&task->wait->io_files);
	RB_CLEAR_NODE(&task->sched->mgr_rb);
	RB_CLEAR_NODE(&task->wait->timer_rb);
	task->wait->wait_interruptible = 0;
	task->wait->signal_wait_mask = 0;
	task->wait->timer_due_ms = 0;
	task->life->fn = fn;
	task->life->param = param;
	task->sched->priority = priority;
	task->life->type = type;
	task->sched->status = ps_ready;
	task->fs->umask = 0;
	task->sched->remain_ticks = DEFAULT_TASK_TIME_SLICE;
	task->life->psid = ps_id_gen();
	task->thread->tgid = task->life->psid;
	task->life->ppid = task->life->psid;
	task->life->exit_signal = SIGCHLD;
	task->magic = 0xdeadbeef;

	task_init_selectors(task);
	arch_task_init_switch_frame(task, (uintptr_t)ps_run, stack_bottom);
	smp_fpu_new(task);

	task->stats = zalloc(sizeof(task_stats_t));
	if (!task->stats) {
		ps_id_free(task->life->psid);
		ps_free_task(task);
		return -ENOMEM;
	}
	task->stats->start_tickets = time_now_tickets();

	spinlock_lock(&ps_lock, &irq);
	ps_put_to_ready_queue_unsafe(task);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
	return task->life->psid;
}

/*
 * Static helpers — file-descriptor duplication
 */

static task_files *ps_alloc_files(void)
{
	task_files *files = zalloc(sizeof(*files));

	if (!files)
		return NULL;
	files->fds = vm_alloc(FD_TABLE_PAGES);
	if (!files->fds) {
		kfree(files);
		return NULL;
	}
	memset(files->fds, 0, MAX_FD * sizeof(file *));
	ref_count_init(&files->ref, ps_release_files);
	mutex_init(&files->lock);
	return files;
}

static int ps_init_fds(task_struct *task)
{
	task_files *files = ps_alloc_files();

	if (!files)
		return -ENOMEM;
	task->files = files;
	return 0;
}

static task_files *ps_copy_files(task_files *old)
{
	task_files *files = ps_alloc_files();
	int i;

	if (!files)
		return NULL;
	mutex_lock(&old->lock);
	for (i = 0; i < MAX_FD; i++) {
		if (!old->fds[i])
			continue;
		files->fds[i] = old->fds[i];
		fs_get_file(old->fds[i]);
	}
	memcpy(files->cloexec, old->cloexec, sizeof(files->cloexec));
	mutex_unlock(&old->lock);
	return files;
}

int ps_dup_fds(task_struct *cur, task_struct *task, int share)
{
	task_files *files;

	if (share) {
		files = ref_count_get(cur->files);
	} else {
		files = ps_copy_files(cur->files);
		if (!files)
			return -ENOMEM;
	}
	task->files = files;
	return 0;
}

static void ps_release_files(ref_count_t *ref)
{
	task_files *files = container_of(ref, task_files, ref);
	int i;
	for (i = 0; i < MAX_FD; i++) {
		if (files->fds[i])
			fs_put_file(files->fds[i]);
	}
	vm_free((vaddr_t)files->fds, FD_TABLE_PAGES);
	kfree(files);
}

void ps_put_fds(task_struct *task)
{
	task_files *files = task->files;

	task->files = NULL;
	if (files)
		ref_count_put(files);
}

int ps_unshare_fds(task_struct *task)
{
	task_files *old = task->files;
	task_files *files;

	if (ref_count_read(old) == 1)
		return 0;
	files = ps_copy_files(old);
	if (!files)
		return -ENOMEM;
	task->files = files;
	ref_count_put(old);
	return 0;
}

/*
 * Static helpers — COW user address-space duplication
 *
 * Mirrors Linux's copy_page_range() / copy_pte_range() / copy_one_pte().
 * VMAs are walked first; for each VMA the page-table entries in its address
 * range are copied with appropriate COW semantics:
 *
 *   MAP_PRIVATE + writable  →  write-protect in parent AND child (COW)
 *   MAP_SHARED  or read-only →  copy PTE as-is (no write-protection)
 *
 * This avoids the spurious write fault that the old flat walk caused for
 * MAP_SHARED writable pages (which it write-protected unconditionally).
 */

struct copy_page_range_ctx {
	pte_t *src_pd;
	pte_t *dst_pd;
	vm_struct_t child_vm;
	int error;
};

static void copy_vma_callback(vm_region *vma, void *data)
{
	struct copy_page_range_ctx *ctx = data;
	if (ctx->error)
		return;
	vm_add_map_clone(ctx->child_vm, vma);
	if (!arch_mm_clone_region(ctx->src_pd, ctx->dst_pd, vma))
		ctx->error = -ENOMEM;
}

/*
 * copy_page_range — set up the child's page tables from the parent.
 *
 * VMAs are duplicated into the child first, then each VMA's PTEs are walked
 * and copied with COW semantics.  A full TLB flush is issued at the end
 * because parent PTEs that were writable may now be read-only in the TLB.
 */
int copy_page_range(task_struct *parent, task_struct *child)
{
	struct copy_page_range_ctx ctx = {
		.src_pd = (pte_t *)mm_get_pagedir(),
		.dst_pd = (pte_t *)child->memory->page_dir,
		.child_vm = child->memory,
	};

	mm_init_process_page_dir((vaddr_t)ctx.dst_pd);
	vm_enum(parent->memory, copy_vma_callback, &ctx);
	smp_tlb_flush_user(parent->memory->page_dir);
	return ctx.error;
}

/*
 * Static helpers — shared between do_fork and do_vfork
 */

/* Allocate the child task page, clone the parent's register state, and set
 * up the kernel stack so the child returns through ret_from_fork. */
task_struct *fork_alloc_child(task_struct *cur)
{
	task_struct *task = vm_alloc(KERNEL_TASK_SIZE);
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
			       sizeof(intr_frame));
	intr_frame *task_intr_frame;

	if (!task)
		return NULL;

	task_intr_frame = (intr_frame *)((char *)task + KERNEL_TASK_BYTES -
					 sizeof(intr_frame));

	smp_fpu_save(cur);
	memset(task, 0, sizeof(*task));
	if (ps_init_private(task)) {
		ps_free_task(task);
		return NULL;
	}
	task->execution->arch = cur->execution->arch;
	task->execution->io_priv_level = cur->execution->io_priv_level;
	task->execution->io_allow_all = cur->execution->io_allow_all;
	task->execution->robust_list_size = cur->execution->robust_list_size;
	task->sched->remain_ticks = cur->sched->remain_ticks;
	task->magic = 0xdeadbeef;
	list_init(&task->wait->io_files);
	list_init(&task->signal->pending_queue);
	*task_intr_frame = *cur_intr_frame;

	if (cur->sched->remain_ticks > DEFAULT_TASK_TIME_SLICE / 2)
		cur->sched->remain_ticks = task->sched->remain_ticks =
			cur->sched->remain_ticks / 2;
	else
		task->sched->remain_ticks = cur->sched->remain_ticks;
	task->life->psid = ps_id_gen();
	task->files = NULL;

	task_init_selectors(task);
	arch_task_copy_user_context(task, cur);
	arch_task_init_switch_frame(task, (uintptr_t)ret_from_fork,
				    (uintptr_t)task_intr_frame);
	task_intr_frame->eax = 0;

	task->life->ppid = cur->life->psid;
	task->life->exit_signal = SIGCHLD;
	task->life->nchildren = 0;
	task->execution->io_bitmap = NULL;
	list_init(&task->sched->ps_list);
	list_init(&task->life->dying_queue);
	list_init(&task->wait->io_files);
	RB_CLEAR_NODE(&task->sched->mgr_rb);
	RB_CLEAR_NODE(&task->wait->timer_rb);
	task->wait->wait_interruptible = 0;
	task->wait->signal_wait_mask = 0;
	task->wait->timer_due_ms = 0;
	return task;
}

/* Copy private execution state and initialize newly copied VM metadata. */
void ps_copy_thread_state(task_struct *cur, task_struct *task)
{
	smp_fpu_copy(cur, task);
	if (task->memory != cur->memory) {
		task->memory->task_size = cur->memory->task_size;
		task->memory->mmap_base = cur->memory->mmap_base;
		task->memory->brk_limit = cur->memory->brk_limit;
		task->memory->start_brk = cur->memory->start_brk;
		task->memory->brk = cur->memory->brk;
		task->memory->start_stack = cur->memory->start_stack;
		task->memory->cmd_len = cur->memory->cmd_len;
		task->memory->env_len = cur->memory->env_len;
		memcpy(task->memory->command, cur->memory->command,
		       cur->memory->cmd_len);
		memcpy(task->memory->environment, cur->memory->environment,
		       cur->memory->env_len);
		task->memory->executable = cur->memory->executable;
		if (task->memory->executable)
			fs_get_file(task->memory->executable);
	}

	*task->credentials = *cur->credentials;
	memcpy(task->execution->tls_desc, cur->execution->tls_desc,
	       sizeof(cur->execution->tls_desc));
	if (task->memory != cur->memory) {
		memcpy(task->memory->ldt_desc, cur->memory->ldt_desc,
		       sizeof(cur->memory->ldt_desc));
		task->memory->ldt_present = cur->memory->ldt_present;
	}
}

/* Duplicate the signal context; child starts with no pending signals. */
int fork_dup_io(task_struct *cur, task_struct *task)
{
	if (!cur->execution->io_bitmap) {
		task->execution->io_bitmap = NULL;
		return 0;
	}
	task->execution->io_bitmap = kmalloc(TSS_IO_BITMAP_BYTES);
	if (!task->execution->io_bitmap)
		return -ENOMEM;
	memcpy(task->execution->io_bitmap, cur->execution->io_bitmap,
	       TSS_IO_BITMAP_BYTES);
	return 0;
}

/* Copy scheduling metadata and ownership fields from parent to child. */
int fork_set_meta(task_struct *cur, task_struct *task, unsigned fork_flag)
{
	task->sched->priority = cur->sched->priority;
	task->life->type = cur->life->type;
	task->life->fork_flag = fork_flag;
	task->stats = zalloc(sizeof(task_stats_t));
	if (!task->stats)
		return -ENOMEM;
	task->stats->start_tickets = time_now_tickets();
	return 0;
}

/* Release a fully prepared child that has not entered the task queues. */
void fork_abort_child(task_struct *task)
{
	ps_id_free(task->life->psid);
	ps_free_task(task);
}

/* Enqueue the child: increment parent's child count and add to ready+mgr. */
void fork_enqueue(task_struct *cur, task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	cur->life->nchildren++;
	ps_put_to_ready_queue_unsafe(task);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

/*
 * Process-style fork/vfork children benefit from running their return path
 * before the parent resumes: a fork child often execs immediately, which can
 * drop the extra COW references before the parent dirties private pages.
 */
void ps_enqueue_child_first(task_struct *cur, task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	cur->life->nchildren++;
	ps_put_to_ready_queue_unsafe(task);
	list_remove_entry(&task->sched->ps_list);
	list_insert_head(&control.ready_queue[task->sched->priority],
			 &task->sched->ps_list);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

/*
 * Public — fork / vfork
 */

/* Core fork implementation. Creates a copy of the current task with COW
 * user address space. */
static int do_fork(void)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
			       sizeof(intr_frame));
	task_struct *task = fork_alloc_child(cur);

	if (!task)
		return -ENOMEM;

	if (ps_clone_resources(cur, task, SIGCHLD) ||
	    fork_set_meta(cur, task, 0)) {
		fork_abort_child(task);
		return -ENOMEM;
	}
	task->thread->tgid = task->life->psid;
	task->life->exit_signal = SIGCHLD;

	ps_enqueue_child_first(cur, task);
	cur_intr_frame->eax = task->life->psid;
	task_sched();
	return task->life->psid;
}

/* vfork blocks the parent until exec/exit. Each clone flag still selects
 * its own resource owner; sys_vfork always requests CLONE_VM. */
int do_vfork(unsigned long child_stack, unsigned long flags)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
			       sizeof(intr_frame));
	task_struct *task = fork_alloc_child(cur);

	if (!task)
		return -ENOMEM;

	if (ps_clone_resources(cur, task, flags) ||
	    fork_set_meta(cur, task, FORK_FLAG_VFORK)) {
		fork_abort_child(task);
		return -ENOMEM;
	}
	task->thread->tgid = task->life->psid;
	task->life->exit_signal = SIGCHLD;
	if (child_stack) {
		intr_frame *task_intr_frame =
			(intr_frame *)((char *)task + KERNEL_TASK_BYTES -
				       sizeof(intr_frame));
		task_intr_frame->esp = (void *)child_stack;
	}

	cond_init(&task->life->vfork_event, 1);
	ps_enqueue_child_first(cur, task);
	cond_wait(&task->life->vfork_event, 0);

	cur_intr_frame->eax = task->life->psid;
	return task->life->psid;
}

int sys_fork()
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("fork()\n");
	return do_fork();
}

int sys_vfork()
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("vfork()\n");
	return do_vfork(0, CLONE_VM | CLONE_VFORK | SIGCHLD);
}
