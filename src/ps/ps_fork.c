/*
 * ps_fork.c — task creation: ps_create, fork, vfork.
 *
 * Owns:
 *   - Kernel task entry point and creation (ps_run, ps_create)
 *   - COW fork (do_fork, sys_fork)
 *   - Real vfork (do_vfork, sys_vfork)
 */

#include "hw/time.h"
#include <hw/cpu.h>
#include <ps/ps.h>
#include <int/int.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <mm/mm.h>
#include <dev/dev.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <errno.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <config.h>
#include <macro.h>

#include "ps_internal.h"

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
	_ps_enabled[ps_sched_cpu()] = 1;
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
unsigned _ps_create_affine(process_fn fn, const char *name, void *param,
			   ps_priority priority, ps_type type, int affinity)
{
	unsigned int stack_bottom;
	task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);
	int irq;

	if (priority >= PS_PRIORITY_MAX) {
		vm_free(task, 1);
		return 0xffffffff;
	}

	memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);

	task->user = ps_alloc_user_env();
	if (!task->user) {
		vm_free(task, 1);
		return -ENOMEM;
	}
	task->user->page_dir = (unsigned int)vm_alloc(1);
	task->user->command = vm_alloc(1);
	task->user->environment = vm_alloc(1);
	sprintf(task->user->command, "sys-%s", name);
	task->user->cmd_len = strlen(task->user->command) + 1;
	*((char *)task->user->environment) = '\0';
	task->user->env_len = 0;
	mm_init_task_pagedir((unsigned int *)task->user->page_dir);
	task->user->cwd = name_get();
	memset(task->user->cwd, 0, MAX_PATH);
	task->user->root_path = name_get();
	strcpy(task->user->root_path, "/");
	task->user->vm = vm_create();
	/* Default rlimits: RLIM_INFINITY for all, except known constraints. */
	for (int i = 0; i < RLIM_NLIMITS; i++) {
		task->user->rlimits[i].rlim_cur = RLIM_INFINITY;
		task->user->rlimits[i].rlim_max = RLIM_INFINITY;
	}
	task->user->rlimits[3].rlim_cur =
		USER_STACK_PAGES * PAGE_SIZE; /* RLIMIT_STACK */
	task->user->rlimits[3].rlim_max = USER_STACK_PAGES * PAGE_SIZE;
	task->user->rlimits[4].rlim_cur = 0; /* RLIMIT_CORE: no core dumps */
	task->user->rlimits[4].rlim_max = 0;
	/* RLIMIT_NOFILE: match Linux default of 1024/1024.  Services that do
	 * malloc(rl_cur * ...) won't OOM, and glibc sizes its fd table to 1024
	 * which matches xinetd's max_descriptors after its setrlimit call. */
	task->user->rlimits[7].rlim_cur = 1024;
	task->user->rlimits[7].rlim_max = 1024;

	task->signal = zalloc(sizeof(signal_context));
	task->io_bitmap = kmalloc(TSS_IO_BITMAP_BYTES);
	if (task->io_bitmap)
		memset(task->io_bitmap, 0xff, TSS_IO_BITMAP_BYTES);

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
	task->tgid = task->psid;
	task->ppid = task->psid;
	task->exit_signal = SIGCHLD;
	task->affinity = affinity >= 0 ? (unsigned)affinity : 0;
	task->fds = vm_alloc(1);
	task->fd_cloexec = zalloc(FD_BITMAP_WORDS * sizeof(unsigned long));
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

	if (task->priority == ps_idle && task->affinity < MAX_CPUS)
		cpus[task->affinity].idle = task;

	spinlock_lock(&ps_lock, &irq);
	ps_put_to_ready_queue_unsafe(task);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
	return task->psid;
}

unsigned _ps_create(process_fn fn, const char *name, void *param,
		    ps_priority priority, ps_type type)
{
	return _ps_create_affine(fn, name, param, priority, type, -1);
}

/*
 * Static helpers — file-descriptor duplication
 */

void ps_dup_fds(task_struct *cur, task_struct *task)
{
	int i;
	memset(task->fds, 0, PAGE_SIZE);
	memset(task->fd_cloexec, 0, FD_BITMAP_WORDS * sizeof(unsigned long));
	mutex_lock(&cur->fd_lock);
	for (i = 0; i < MAX_FD; i++) {
		if (!cur->fds[i])
			continue;
		task->fds[i] = cur->fds[i];
		fs_get_file(cur->fds[i]);
		if (fd_bitmap_test(cur->fd_cloexec, i))
			fd_bitmap_set(task->fd_cloexec, i);
	}
	mutex_unlock(&cur->fd_lock);
}

int do_vfork(void);

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
	unsigned phy = pte & PAGE_SIZE_MASK;
	unsigned page_index;

	if (!(pte & PAGE_ENTRY_PRESENT))
		return;

	/*
	 * /dev/mem mappings may point at MMIO or reserved physical ranges such
	 * as the linear framebuffer BAR at 0xFD000000.  Those pages are not
	 * owned by phymm, so fork must clone the PTE without taking a RAM page
	 * reference or forcing COW semantics.
	 */
	if (dev_mem_is_file(vma->fp)) {
		if ((*dst_pte & PAGE_SIZE_MASK) == 0)
			(*pt_count)++;
		*dst_pte = pte;
		return;
	}

	page_index = phy / PAGE_SIZE;

	/*
	 * Be defensive for any PTE that points outside allocator-managed RAM or
	 * into a reserved hole.  Those mappings behave like MMIO/firmware pages:
	 * clone the PTE, but never feed the address into phymm reference counts.
	 */
	if (page_index < phymm_begin || page_index >= phymm_end ||
	    phymm_pages[page_index].ref_count == PHYMM_RESERVED) {
		if ((*dst_pte & PAGE_SIZE_MASK) == 0)
			(*pt_count)++;
		*dst_pte = pte;
		return;
	}

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
void copy_page_range(task_struct *parent, task_struct *child)
{
	struct copy_page_range_ctx ctx = {
		.src_pd = (unsigned *)mm_get_pagedir(),
		.dst_pd = (unsigned *)child->user->page_dir,
	};
	int irq;

	mm_init_task_pagedir(ctx.dst_pd);
	vm_dup(parent->user->vm, child->user->vm);

	/*
	 * Serialize fork's parent-PTE write-protect pass against concurrent COW
	 * faults and other page-table writers touching the same address space.
	 */
	spinlock_lock(&map_lock, &irq);
	vm_enum(parent->user->vm, copy_vma_callback, &ctx);
	RELOAD_CR3();
	spinlock_unlock(&map_lock, irq);
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
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	intr_frame *task_intr_frame;

	if (!task)
		return NULL;

	task_intr_frame =
		(intr_frame *)((char *)task + PAGE_SIZE - sizeof(intr_frame));

	*task = *cur;
	*task_intr_frame = *cur_intr_frame;

	if (cur->remain_ticks > DEFAULT_TASK_TIME_SLICE / 2)
		cur->remain_ticks = task->remain_ticks = cur->remain_ticks / 2;
	else
		task->remain_ticks = cur->remain_ticks;
	task->psid = ps_id_gen();
	task->tgid = cur->tgid;
	/* Start fork/clone children on the parent's CPU so the copied user
	 * return frame (especially TLS selector state like %gs) is first
	 * restored on the CPU that already ran the parent. */
	task->affinity = cur->affinity;
	mutex_init(&task->fd_lock);

	task_init_selectors(task);
	task->tss.eax = 0;
	task->tss.ebp = (char *)task_intr_frame;
	task->tss.esp = (char *)task_intr_frame;
	task->tss.esp0 = (unsigned)task + PAGE_SIZE;
	task->tss.eip = (unsigned)ret_from_fork;
	task_intr_frame->eax = 0;

	task->ppid = cur->psid;
	task->exit_signal = SIGCHLD;
	task->nchildren = 0;
	task->fd_cloexec = NULL;
	task->io_bitmap = NULL;
	list_init(&task->ps_list);
	RB_CLEAR_NODE(&task->mgr_rb);
	RB_CLEAR_NODE(&task->timer_rb);
	task->timer_due_ms = 0;
	return task;
}

/* Copy the user environment fields that are identical for both fork and
 * vfork: address-space bookkeeping, command line, environment string, cwd,
 * and all credentials. */
void fork_dup_user_env(task_struct *cur, task_struct *task)
{
	task->user->heap->start_brk = cur->user->heap->start_brk;
	task->user->heap->brk = cur->user->heap->brk;
	task->user->stack_bottom = cur->user->stack_bottom;
	task->user->mmap_cache = NULL;

	task->user->command = vm_alloc(1);
	task->user->cmd_len = cur->user->cmd_len;
	task->user->environment = vm_alloc(1);
	task->user->env_len = cur->user->env_len;
	memcpy(task->user->command, cur->user->command, cur->user->cmd_len);
	memcpy(task->user->environment, cur->user->environment,
	       cur->user->env_len);
	task->user->cwd = name_get();
	strcpy(task->user->cwd, cur->user->cwd);
	task->user->root_path = name_get();
	strcpy(task->user->root_path, cur->user->root_path);
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
	memcpy(task->user->ldt_desc, cur->user->ldt_desc,
	       sizeof(cur->user->ldt_desc));
	memcpy(task->user->rlimits, cur->user->rlimits,
	       sizeof(cur->user->rlimits));
}

/* Duplicate the signal context; child starts with no pending signals. */
void fork_dup_signal(task_struct *cur, task_struct *task)
{
	task->signal = zalloc(sizeof(signal_context));
	memcpy(task->signal, cur->signal, sizeof(signal_context));
	task->signal->sig_pending = 0;
}

int fork_dup_io(task_struct *cur, task_struct *task)
{
	task->io_bitmap = kmalloc(TSS_IO_BITMAP_BYTES);
	if (!task->io_bitmap)
		return -ENOMEM;
	memcpy(task->io_bitmap, cur->io_bitmap, TSS_IO_BITMAP_BYTES);
	return 0;
}

/* Copy scheduling metadata and ownership fields from parent to child. */
void fork_set_meta(task_struct *cur, task_struct *task, unsigned fork_flag)
{
	task->umask = cur->umask;
	task->priority = cur->priority;
	task->type = cur->type;
	task->fork_flag = fork_flag;
	/*
	 * Interval timers are per-process and must not survive into the child.
	 * We clone the full task_struct up front, so reset the inherited alarm
	 * state here before the task becomes runnable.
	 */
	task->alarm_expire_ms = 0;
	task->alarm_interval_ms = 0;
	task->root = cur->root;
	task->stats = zalloc(sizeof(task_stats_t));
	task->stats->start_tickets = time_now_tickets();
	sb_get(task->root);
}

/* Enqueue the child: increment parent's child count and add to ready+mgr. */
void fork_enqueue(task_struct *cur, task_struct *task)
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

	if (!task)
		return -ENOMEM;

	task->user = ps_alloc_user_env();
	if (!task->user)
		return -ENOMEM;
	task->user->vm = vm_create();
	task->user->page_dir = vm_alloc(1);
	mm_init_process_page_dir(task->user->page_dir);
	fork_dup_user_env(cur, task);
	fork_dup_signal(cur, task);
	if (fork_dup_io(cur, task) != 0)
		return -ENOMEM;
	fork_set_meta(cur, task, 0);
	task->tgid = task->psid;
	task->exit_signal = SIGCHLD;

	task->fds = vm_alloc(1);
	task->fd_cloexec = zalloc(FD_BITMAP_WORDS * sizeof(unsigned long));
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
int do_vfork(void)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	task_struct *task = fork_alloc_child(cur);

	if (!task)
		return -ENOMEM;

	task->user = ps_alloc_user_env();
	if (!task->user)
		return -ENOMEM;

	/*
	 * vfork shares the parent's exact address space until exec/exit.
	 * Keep the child on the same CPU so page-table and TLB assumptions that
	 * rely on local invalidation do not suddenly become cross-CPU.
	 */
	task->affinity = cur->affinity;
	/* Borrow parent's address space — child does not own these. */
	task->user->page_dir = cur->user->page_dir;
	task->user->vm = cur->user->vm;
	fork_dup_user_env(cur, task);
	ps_share_heap_state(task->user, cur->user);
	fork_dup_signal(cur, task);
	if (fork_dup_io(cur, task) != 0)
		return -ENOMEM;
	fork_set_meta(cur, task, FORK_FLAG_VFORK);
	task->tgid = task->psid;
	task->exit_signal = SIGCHLD;

	task->fds = vm_alloc(1);
	task->fd_cloexec = zalloc(FD_BITMAP_WORDS * sizeof(unsigned long));
	ps_dup_fds(cur, task);
	fork_enqueue(cur, task);
	cond_init(&task->vfork_event, 1);
	cond_wait(&task->vfork_event, 0);

	cur_intr_frame->eax = task->psid;
	return task->psid;
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
	return do_vfork();
}
