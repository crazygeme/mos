/*
 * ps_fork.c — task creation: ps_create, fork, vfork.
 *
 * Owns:
 *   - Kernel task entry point and creation (ps_run, ps_create)
 *   - COW fork (do_fork, sys_fork)
 *   - Real vfork (do_vfork, sys_vfork)
 */

#include "hw/time.h"
#include <ps/ps.h>
#include <int/int.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/mm.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <config.h>
#include <macro.h>

#include "ps_internal.h"

extern void ret_from_fork();
extern short pgc_entry_count[1024];

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
	task->status = ps_running;
	fn = task->fn;
	if (fn)
		fn(task->param);

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
	unsigned int stack_bottom;
	task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);
	int irq;

	if (priority >= PS_PRIORITY_MAX) {
		vm_free(task, 1);
		return 0xffffffff;
	}

	memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);
	task->user = zalloc(sizeof(user_enviroment));
	task->user->page_dir = (unsigned int)vm_alloc(1);
	task->user->command = vm_alloc(1);
	task->user->environment = vm_alloc(1);
	sprintf(task->user->command, "sys-%s", name);
	task->user->cmd_len = strlen(task->user->command) + 1;
	*((char *)task->user->environment) = '\0';
	task->user->env_len = 0;
	memset(task->user->page_dir, 0, PAGE_SIZE);
	task->user->cwd = name_get();
	memset(task->user->cwd, 0, MAX_PATH);
	task->user->start_brk = 0;
	task->user->brk = 0;
	task->user->vm = vm_create();

	task->signal = zalloc(sizeof(signal_context));

	task->umask = 0;
	stack_bottom = (unsigned int)task + PAGE_SIZE;
	LOAD_CR3(task->cr3);
	list_init(&task->ps_list);
	RB_CLEAR_NODE(&task->mgr_rb);
	RB_CLEAR_NODE(&task->timer_rb);
	task->timer_due_ms = 0;
	task->fn = fn;
	task->param = param;
	task->priority = priority;
	task->type = type;
	task->status = ps_ready;
	task->umask = 0;
	task->remain_ticks = DEFAULT_TASK_TIME_SLICE;
	task->psid = ps_id_gen();
	task->parent = task;
	task->fds = vm_alloc(1);
	memset(task->fds, 0, PAGE_SIZE);
	mutex_init(&task->fd_lock);
	task->magic = 0xdeadbeef;

	task_init_selectors(task);
	task->tss.ebp = stack_bottom;
	task->tss.esp = stack_bottom;
	task->tss.esp0 = stack_bottom;
	task->tss.eip = ps_run;

	task->stats = zalloc(sizeof(task_stats_t));
	task->stats->start_tickets = time_now_tickets();

	spinlock_lock(&ps_lock, &irq);
	ps_put_to_ready_queue_unsafe(task);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
	return task->psid;
}

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

/*
 * copy_one_pte — share a single PTE from parent to child.
 *
 * For MAP_PRIVATE writable pages the parent's PTE is cleared of WRITABLE
 * so that the first write by either process triggers a #PF → wp_page_copy().
 * The child inherits the (now read-only) PTE and the page ref_count is bumped.
 */
static void copy_one_pte(unsigned *src_pte, unsigned *dst_pte, vm_region *vma,
			 short *pt_count)
{
	unsigned pte = *src_pte;
	unsigned page_index;

	if (!(pte & PAGE_ENTRY_PRESENT))
		return;

	page_index = (pte & PAGE_SIZE_MASK) / PAGE_SIZE;

	if (!(vma->flag & MAP_SHARED) && (pte & PAGE_ENTRY_WRITABLE)) {
		pte &= ~PAGE_ENTRY_WRITABLE;
		*src_pte = pte; /* write-protect parent */
	}

	if ((*dst_pte & PAGE_SIZE_MASK) == 0)
		(*pt_count)++;

	*dst_pte = pte;
	phymm_reference_page(page_index);
}

/*
 * copy_pte_range — copy all present PTEs within [vma->begin, vma->end)
 * that reside in the page table at src_pd[pde_idx].
 */
static void copy_pte_range(unsigned *src_pd, unsigned *dst_pd, vm_region *vma,
			   unsigned pde_idx)
{
	unsigned *src_pt;
	unsigned *dst_pt;
	unsigned pde_base = pde_idx << 22;
	unsigned pde_end = pde_base + (1u << 22);
	unsigned pt_start =
		(vma->begin > pde_base) ? ADDR_TO_PET_OFFSET(vma->begin) : 0;
	unsigned pt_end =
		(vma->end >= pde_end) ?
			1024 :
			ADDR_TO_PET_OFFSET((vma->end - PAGE_SIZE)) + 1;
	unsigned pde_flag = src_pd[pde_idx] & ~PAGE_SIZE_MASK;
	int cache_idx;
	unsigned i;

	src_pt = (unsigned *)((src_pd[pde_idx] & PAGE_SIZE_MASK) +
			      KERNEL_OFFSET);

	if (!(dst_pd[pde_idx] & PAGE_SIZE_MASK))
		dst_pd[pde_idx] = (mm_alloc_page_table() - KERNEL_OFFSET) |
				  pde_flag;

	dst_pt = (unsigned *)((dst_pd[pde_idx] & PAGE_SIZE_MASK) +
			      KERNEL_OFFSET);
	cache_idx = (PAGE_TABLE_CACHE_END - (unsigned)dst_pt) / PAGE_SIZE - 1;

	for (i = pt_start; i < pt_end; i++) {
		if (!(src_pt[i] & PAGE_ENTRY_PRESENT))
			continue;
		copy_one_pte(&src_pt[i], &dst_pt[i], vma,
			     &pgc_entry_count[cache_idx]);
	}
}

/*
 * copy_vma_pages — copy all PTEs for one VMA.
 *
 * Iterates only the PDE indices covered by the VMA; entries that are not
 * present (pages never faulted in) are skipped without descending.
 */
static void copy_vma_pages(unsigned *src_pd, unsigned *dst_pd, vm_region *vma)
{
	unsigned pde_first = ADDR_TO_PGT_OFFSET(vma->begin);
	unsigned pde_last = ADDR_TO_PGT_OFFSET((vma->end - PAGE_SIZE));
	unsigned pde_idx;

	for (pde_idx = pde_first; pde_idx <= pde_last; pde_idx++) {
		if (!(src_pd[pde_idx] & PAGE_SIZE_MASK))
			continue;
		copy_pte_range(src_pd, dst_pd, vma, pde_idx);
	}
}

struct copy_page_range_ctx {
	unsigned *src_pd;
	unsigned *dst_pd;
};

static void copy_vma_callback(vm_region *vma, void *data)
{
	struct copy_page_range_ctx *ctx = data;
	copy_vma_pages(ctx->src_pd, ctx->dst_pd, vma);
}

/*
 * copy_page_range — set up the child's page tables from the parent.
 *
 * VMAs are duplicated into the child first, then each VMA's PTEs are walked
 * and copied with COW semantics.  A full TLB flush is issued at the end
 * because parent PTEs that were writable may now be read-only in the TLB.
 */
static void copy_page_range(task_struct *parent, task_struct *child)
{
	struct copy_page_range_ctx ctx = {
		.src_pd = (unsigned *)mm_get_pagedir(),
		.dst_pd = (unsigned *)child->user->page_dir,
	};

	memset(ctx.dst_pd, 0, PAGE_SIZE);
	vm_dup(parent->user->vm, child->user->vm);
	vm_enum(parent->user->vm, copy_vma_callback, &ctx);
	RELOAD_CR3();
}

/*
 * Static helpers — shared between do_fork and do_vfork
 */

/* Allocate the child task page, clone the parent's register state, and set
 * up the kernel stack so the child returns through ret_from_fork. */
static task_struct *fork_alloc_child(task_struct *cur)
{
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
	task->psid = ps_id_gen();
	mutex_init(&task->fd_lock);

	task_init_selectors(task);
	task->tss.eax = 0;
	task->tss.ebp = (char *)task_intr_frame;
	task->tss.esp = (char *)task_intr_frame;
	task->tss.esp0 = (unsigned)task + PAGE_SIZE;
	task->tss.eip = (unsigned)ret_from_fork;
	task_intr_frame->eax = 0;

	task->parent = cur;
	task->nchildren = 0;
	list_init(&task->ps_list);
	RB_CLEAR_NODE(&task->mgr_rb);
	RB_CLEAR_NODE(&task->timer_rb);
	task->timer_due_ms = 0;
	return task;
}

/* Copy the user environment fields that are identical for both fork and
 * vfork: address-space bookkeeping, command line, environment string, cwd,
 * and all credentials. */
static void fork_dup_user_env(task_struct *cur, task_struct *task)
{
	task->user->brk = cur->user->brk;
	task->user->start_brk = cur->user->start_brk;
	task->user->stack_bottom = cur->user->stack_bottom;

	task->user->command = vm_alloc(1);
	task->user->cmd_len = cur->user->cmd_len;
	task->user->environment = vm_alloc(1);
	task->user->env_len = cur->user->env_len;
	memcpy(task->user->command, cur->user->command, cur->user->cmd_len);
	memcpy(task->user->environment, cur->user->environment,
	       cur->user->env_len);
	task->user->cwd = name_get();
	strcpy(task->user->cwd, cur->user->cwd);
	task->user->group_id = cur->user->group_id;
	task->user->session_id = cur->user->session_id;
	task->user->uid = cur->user->uid;
	task->user->euid = cur->user->euid;
	task->user->suid = cur->user->suid;
	task->user->gid = cur->user->gid;
	task->user->egid = cur->user->egid;
	task->user->sgid = cur->user->sgid;
	task->user->fsuid = cur->user->fsuid;
	task->user->fsgid = cur->user->fsgid;
	memcpy(task->user->tls_desc, cur->user->tls_desc,
	       sizeof(cur->user->tls_desc));
}

/* Duplicate the signal context; child starts with no pending signals. */
static void fork_dup_signal(task_struct *cur, task_struct *task)
{
	task->signal = zalloc(sizeof(signal_context));
	memcpy(task->signal, cur->signal, sizeof(signal_context));
	task->signal->sig_pending = 0;
}

/* Copy scheduling metadata and ownership fields from parent to child. */
static void fork_set_meta(task_struct *cur, task_struct *task,
			  unsigned fork_flag)
{
	task->umask = cur->umask;
	task->priority = cur->priority;
	task->type = cur->type;
	task->fork_flag = fork_flag;
	task->root = cur->root;
	task->stats = zalloc(sizeof(task_stats_t));
	task->stats->start_tickets = time_now_tickets();
	sb_get(task->root);
}

/* Enqueue the child: increment parent's child count and add to ready+mgr. */
static void fork_enqueue(task_struct *cur, task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	cur->nchildren++;
	ps_put_to_ready_queue_unsafe(task);
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
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	task_struct *task = fork_alloc_child(cur);

	task->user = zalloc(sizeof(user_enviroment));
	task->user->vm = vm_create();
	task->user->page_dir = vm_alloc(1);
	fork_dup_user_env(cur, task);
	fork_dup_signal(cur, task);
	fork_set_meta(cur, task, 0);

	task->fds = vm_alloc(1);
	ps_dup_fds(cur, task);
	copy_page_range(cur, task);
	fork_enqueue(cur, task);
	cur_intr_frame->eax = task->psid;
	return task->psid;
}

/*
 * Real vfork: child shares the parent's address space (same page_dir and vm)
 * without any copying.  Parent blocks until child calls exec() or exit().
 *
 * The child does NOT own page_dir or vm — cleanup() and do_exit() detect
 * FORK_FLAG_VFORK and skip the destroy/unmap paths for those resources.
 */
static int do_vfork(void)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	task_struct *task = fork_alloc_child(cur);

	task->user = zalloc(sizeof(user_enviroment));
	/* Borrow parent's address space — child does not own these. */
	task->user->page_dir = cur->user->page_dir;
	task->user->vm = cur->user->vm;
	fork_dup_user_env(cur, task);
	fork_dup_signal(cur, task);
	fork_set_meta(cur, task, FORK_FLAG_VFORK);

	task->fds = vm_alloc(1);
	ps_dup_fds(cur, task);
	fork_enqueue(cur, task);

	cond_init(&task->vfork_event, 1);
	cond_wait(&task->vfork_event, 0);

	cur_intr_frame->eax = task->psid;
	return task->psid;
}

int sys_fork()
{
	if (TestControl.verbos)
		klog("fork()\n");
	return do_fork();
}

int sys_vfork()
{
	if (TestControl.verbos)
		klog("vfork()\n");
	return do_vfork();
}
