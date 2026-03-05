/*
 * ps.c — process/task management and scheduler
 *
 * Responsibilities:
 *   - Task creation (ps_create), fork (do_fork), and exit (sys_exit)
 *   - Scheduler: Multilevel Priority Ready Queue backed by per-level RB-trees
 *   - Context switch (_task_sched / SAVE_ALL / RESTORE_ALL)
 *   - Queue management: ready, wait, dying
 *   - waitpid / reaping of dead children
 *   - User address-space duplication (COW fork)
 */

#include <mount.h>
#include <ptrace.h>
#include <block.h>
#include <mmap.h>
#include <phymm.h>
#include <time.h>
#include <ps.h>
#include <lock.h>
#include <mm.h>
#include <klib.h>
#include <config.h>
#include <int.h>
#include <list.h>
#include <fs.h>
#include <dsr.h>
#include <macro.h>
#include <port.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */

/* Global scheduler / queue control block. */
typedef struct _ps_control {
	struct rb_root
		ready_queue[MAX_PRIORITY]; /* per-priority runnable tasks */
	list_entry dying_queue; /* tasks that have exited, awaiting waitpid */
	list_entry wait_queue; /* tasks blocked on a lock or waitpid */
	list_entry mgr_queue; /* all live tasks (for lookup / enumeration) */
	task_struct *ps_dsr; /* pending deferred service routine task */
} ps_control;

static ps_control control;

/* Fine-grained locks: each queue has its own spinlock to reduce contention.
 * Lock ordering (always acquire in this order to avoid deadlock):
 *   dying_queue_lock → ps_lock
 *   wait_queue_lock  → ps_lock   (wait queue callers may subsequently wake
 *                                  a task via ps_put_to_ready_queue)
 * mgr_queue_lock is independent of the others. */
static spinlock_t ps_lock; /* guards ready_queue */
static spinlock_t psid_lock; /* guards ps_id counter */
static spinlock_t dying_queue_lock; /* guards dying_queue */
static spinlock_t wait_queue_lock; /* guards wait_queue */
static spinlock_t mgr_queue_lock; /* guards mgr_queue */
static spinlock_t map_lock; /* guards per-process page-table ops */

static unsigned int ps_id;
static int _ps_enabled = 0;

/* Scheduling instrumentation: cumulative time (µs) spent inside _task_sched. */
unsigned long long task_schedule_time = 0;
unsigned task_schedule_count = 0;
static unsigned long long sched_begin = 0;
static unsigned long long sched_end = 0;

static tss_struct *tss_address = 0;

/* -------------------------------------------------------------------------
 * Manager queue helpers
 * All tasks, from creation until fully reaped, live in mgr_queue.
 * This is the authoritative list for ps_find_process().
 * ------------------------------------------------------------------------- */

static void ps_add_mgr(task_struct *task)
{
	spinlock_lock(&mgr_queue_lock);
	list_insert_tail(&control.mgr_queue, &task->ps_mgr);
	spinlock_unlock(&mgr_queue_lock);
}

static void ps_remove_mgr(task_struct *task)
{
	spinlock_lock(&mgr_queue_lock);
	list_remove_entry(&task->ps_mgr);
	spinlock_unlock(&mgr_queue_lock);
}

/* Wake the process with the given pid by moving it to the ready queue.
 * Used to notify a parent that one of its children has changed state. */
static void ps_notify_process(unsigned pid)
{
	task_struct *task = ps_find_process(pid);
	if (task) {
		ps_put_to_ready_queue(task);
	}
}

/* Linear search of mgr_queue by psid.  Returns NULL if not found.
 * Callers should not hold mgr_queue_lock. */
task_struct *ps_find_process(unsigned psid)
{
	list_entry *head = &control.mgr_queue;
	list_entry *entry;
	task_struct *task = 0;

	spinlock_lock(&mgr_queue_lock);

	entry = head->prev;
	while (entry != head) {
		task = container_of(entry, task_struct, ps_mgr);
		if (task->psid == psid) {
			break;
		} else {
			task = 0;
		}
		entry = entry->prev;
	}
	spinlock_unlock(&mgr_queue_lock);

	return task;
}

/* -------------------------------------------------------------------------
 * Scheduler: Multilevel Priority Ready Queue (MPRQ)
 *
 * MAX_PRIORITY RB-trees, one per priority level (0 = lowest).
 * Within each level tasks are ordered by sched_seq (monotonically
 * increasing), giving FIFO ordering within a priority class.
 *
 * Selection algorithm:
 *   for p = MAX_PRIORITY-1 downto 0:
 *     walk rb_first → rb_next; skip sleeping (timeout > now) / dying tasks
 *     on first valid task: erase, re-insert with fresh sched_seq
 *       (moves it to the back of its level for round-robin fairness)
 *     return it
 *   if no runnable task found: return current (self-switch → no-op)
 *
 * Complexity: O(log n) insert, O(log n + k) pick (k = sleeping tasks at front).
 * ------------------------------------------------------------------------- */

/* Monotonically increasing tick; lower sched_seq = older = runs first. */
static unsigned long sched_clock = 0;

/* Insert task into a per-priority RB-tree ordered by sched_seq.
 * Must be called with ps_lock held. */
static void ready_queue_insert(struct rb_root *root, task_struct *task)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;

	while (*link) {
		task_struct *t = rb_entry(*link, task_struct, rb_node);
		parent = *link;
		link = (task->sched_seq < t->sched_seq) ? &(*link)->rb_left :
							  &(*link)->rb_right;
	}
	rb_link_node(&task->rb_node, parent, link);
	rb_insert_color(&task->rb_node, root);
}

/* Remove task from its priority-level RB-tree if it is currently enqueued.
 * Acquires ps_lock internally; must NOT be called with ps_lock already held. */
static void ps_remove_from_ready(task_struct *task)
{
	spinlock_lock(&ps_lock);
	if (!RB_EMPTY_NODE(&task->rb_node)) {
		rb_erase(&task->rb_node, &control.ready_queue[task->priority]);
		RB_CLEAR_NODE(&task->rb_node);
	}
	spinlock_unlock(&ps_lock);
}

/* -------------------------------------------------------------------------
 * Queue transition functions
 * Each function moves a task atomically from wherever it currently lives
 * into the target queue.
 * ------------------------------------------------------------------------- */

/* Move task to the dying queue and notify its parent.
 * The task's resources (page dir, command, etc.) are NOT freed here; that
 * is deferred to the parent's waitpid call so that this syscall path
 * remains interruptible.
 *
 * The status is set to ps_dying only after the task is enqueued so that a
 * preemption between the two steps cannot cause the task to be lost:  a
 * ps_dying task is skipped by the scheduler, so if it were marked dying
 * before being enqueued it would never appear in the dying queue. */
void ps_put_to_dying_queue(task_struct *task)
{
	ps_remove_from_ready(task);
	spinlock_lock(&dying_queue_lock);
	if (task->ps_list.prev && task->ps_list.next)
		list_remove_entry(&task->ps_list);
	if (task->psid != 0xffffffff)
		list_insert_tail(&control.dying_queue, &task->ps_list);
	task->status = ps_dying;
	ps_notify_process(task->parent);
	spinlock_unlock(&dying_queue_lock);
}

/* Move task to the wait queue (blocked, e.g. waiting on a mutex or waitpid).
 * The caller is responsible for re-queuing the task when the wait condition
 * is satisfied (via ps_put_to_ready_queue). */
void ps_put_to_wait_queue(task_struct *task)
{
	ps_remove_from_ready(task);
	spinlock_lock(&wait_queue_lock);
	if (task->ps_list.prev && task->ps_list.next)
		list_remove_entry(&task->ps_list);
	if (task->psid != 0xffffffff)
		list_insert_tail(&control.wait_queue, &task->ps_list);
	spinlock_unlock(&wait_queue_lock);
}

/* Enqueue task in the ready queue at its current priority.
 * If the task was in the wait queue (ps_list linked), remove it first.
 * DSR tasks are handled specially: they are not enqueued but recorded
 * directly so the scheduler can dispatch them immediately. */
void ps_put_to_ready_queue(task_struct *task)
{
	spinlock_lock(&ps_lock);
	if (task->type == ps_dsr) {
		control.ps_dsr = task;
	} else if (task->psid != 0xffffffff) {
		if (task->ps_list.prev && task->ps_list.next)
			list_remove_entry(&task->ps_list);
		if (!RB_EMPTY_NODE(&task->rb_node))
			rb_erase(&task->rb_node,
				 &control.ready_queue[task->priority]);
		task->sched_seq = ++sched_clock;
		RB_CLEAR_NODE(&task->rb_node);
		ready_queue_insert(&control.ready_queue[task->priority], task);
	}
	spinlock_unlock(&ps_lock);
}

/* Return the first runnable task from the given priority-level RB-tree.
 * Skips sleeping (timeout in the future) and dying tasks.
 * On success, re-inserts the task with a fresh sched_seq so it goes to the
 * back of the level (round-robin within a priority).
 * Must be called with ps_lock held. */
static task_struct *ps_get_available_ready_task(struct rb_root *root)
{
	struct rb_node *node = rb_first(root);
	unsigned now = time_now_ms();

	while (node) {
		task_struct *task = rb_entry(node, task_struct, rb_node);
		if (task->status != ps_dying && task->timeout <= now) {
			rb_erase(node, root);
			task->sched_seq = ++sched_clock;
			RB_CLEAR_NODE(&task->rb_node);
			ready_queue_insert(root, task);
			return task;
		}
		node = rb_next(node);
	}
	return NULL;
}

/* Select the next task to run.  Checks DSR first (highest priority),
 * then scans ready levels from highest to lowest. */
static task_struct *ps_get_next_task()
{
	task_struct *task = NULL;
	int i = MAX_PRIORITY - 1;

	if (dsr_has_task()) {
		return control.ps_dsr;
	}

	spinlock_lock(&ps_lock);
	for (; i >= 0; i--) {
		if (RB_EMPTY_ROOT(&control.ready_queue[i]))
			continue;
		task = ps_get_available_ready_task(&control.ready_queue[i]);
		if (task)
			break;
	}
	spinlock_unlock(&ps_lock);
	return task;
}

/* -------------------------------------------------------------------------
 * Scheduling instrumentation
 * ------------------------------------------------------------------------- */

static void sched_cal_begin()
{
	sched_begin = time_now_us();
}

static void sched_cal_end()
{
	sched_end = time_now_us();
	if (sched_begin)
		task_schedule_time += (sched_end - sched_begin);
	sched_begin = sched_end = 0;
	task_schedule_count++;
}

/* -------------------------------------------------------------------------
 * Task entry point
 * ------------------------------------------------------------------------- */

/* Every kernel task starts here.  Enables interrupts, runs the task
 * function, then moves the task to the dying queue and yields. */
static void ps_run()
{
	task_struct *task;
	process_fn fn;

	int_intr_enable();
	_ps_enabled = 1;
	task = CURRENT_TASK();
	task->status = ps_running;
	task->is_switching = 0;
	fn = task->fn;
	if (fn)
		fn(0);

	task->status = ps_dying;
	ps_put_to_dying_queue(task);
	ps_remove_mgr(task);
	task_sched();
}

/* -------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------- */

/* Push one updated kernel PDE to every live task's page directory.
 * Called by mm.c (via the registered propagator) when a new kernel PDE is
 * allocated — typically only when a fresh 4 MB kernel region is first used. */
static void ps_propagate_kernel_pde(unsigned offset, unsigned value)
{
	list_entry *head = &control.mgr_queue;
	list_entry *entry;

	spinlock_lock(&mgr_queue_lock);
	entry = head->prev;
	while (entry != head) {
		task_struct *task = container_of(entry, task_struct, ps_mgr);
		if (task->user.page_dir)
			((unsigned int *)task->user.page_dir)[offset] = value;
		entry = entry->prev;
	}
	spinlock_unlock(&mgr_queue_lock);
}

void ps_init()
{
	int i = 0;
	list_init(&control.dying_queue);
	list_init(&control.wait_queue);
	list_init(&control.mgr_queue);
	for (i = 0; i < MAX_PRIORITY; i++)
		control.ready_queue[i].rb_node = NULL;

	spinlock_init(&ps_lock);
	spinlock_init(&psid_lock);
	spinlock_init(&dying_queue_lock);
	spinlock_init(&wait_queue_lock);
	spinlock_init(&mgr_queue_lock);
	spinlock_init(&map_lock);

	ps_id = 0;
	_ps_enabled = 0;
	task_schedule_time = 0;
	sched_begin = sched_end = 0;
	sched_clock = 0;

	tss_address = kmalloc(sizeof(tss_struct));
	int_update_tss((unsigned int)tss_address);

	/* Register the kernel PDE propagator so mm.c can notify ps.c when a
	 * new kernel page-directory entry is created, without a direct
	 * mm → ps dependency. */
	mm_set_kernel_pde_propagator(ps_propagate_kernel_pde);
}

int ps_enabled()
{
	return _ps_enabled;
}

/* -------------------------------------------------------------------------
 * Process ID generation
 * ------------------------------------------------------------------------- */

static unsigned last_gen_pid = (unsigned)0;

/* Atomically allocate and return the next unique process ID. */
unsigned ps_id_gen()
{
	return __sync_fetch_and_add(&last_gen_pid, 1);
}

/* -------------------------------------------------------------------------
 * Task creation
 * ------------------------------------------------------------------------- */

/* Populate the saved register state so that when the task is first scheduled
 * it begins executing at eip with the given stack and segment registers. */
static void ps_setup_task_frame(task_struct *task, unsigned data_seg,
				unsigned eax, unsigned ebx, unsigned ecx,
				unsigned edx, unsigned ebp, unsigned esp,
				unsigned esp0, unsigned eip)
{
	task->tss.fs = task->tss.gs = task->tss.es = task->tss.ss =
		task->tss.ds = data_seg;
	task->tss.cs = KERNEL_CODE_SELECTOR;
	task->tss.eax = eax;
	task->tss.ebx = ebx;
	task->tss.ecx = ecx;
	task->tss.edx = edx;
	task->tss.ebp = ebp;
	task->tss.esp = esp;
	task->tss.esp0 = esp0;
	task->tss.eip = eip;
}

/* Allocate and initialise a new kernel task.  The task is immediately placed
 * in the ready queue.  Returns the new psid, or 0xffffffff on failure. */
unsigned ps_create(process_fn fn, int priority, ps_type type)
{
	unsigned int stack_buttom;
	int size = 0;
	task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);

	if (priority >= MAX_PRIORITY) {
		vm_free(task, 1);
		return 0xffffffff;
	}

	memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);
	size = sizeof(*task);

	task->user.page_dir = (unsigned int)vm_alloc(1);
	task->command = vm_alloc(1);
	task->umask = 0;
	strcpy(task->command, "system");
	/* Zero the user portion; populate the kernel portion from the global
	 * kernel page directory so this task immediately shares all current
	 * kernel page tables with no per-switch copy needed. */
	memset((void *)task->user.page_dir, 0,
	       KERNEL_PAGE_DIR_OFFSET * sizeof(unsigned int));
	mm_copy_kernel_pgd((unsigned int *)task->user.page_dir);

	stack_buttom = (unsigned int)task + KERNEL_TASK_SIZE * PAGE_SIZE - 4;
	LOAD_CR3(task->cr3);
	task->ps_list.prev = task->ps_list.next = 0;
	RB_CLEAR_NODE(&task->rb_node);
	task->sched_seq = 0;
	task->fn = fn;
	task->priority = priority;
	task->type = type;
	task->status = ps_ready;
	task->remain_ticks = DEFAULT_TASK_TIME_SLICE;
	task->timeout = 0;
	task->psid = ps_id_gen();
	task->is_switching = 0;
	task->fds = vm_alloc(1);
	task->cwd = name_get();
	memset(task->fds, 0, PAGE_SIZE);
	task->user.heap_top = USER_HEAP_BEGIN;
	task->user.vm = vm_create();
	memset(task->cwd, 0, MAX_PATH);

	mutex_init(&task->fd_lock);
	task->magic = 0xdeadbeef;

	ps_setup_task_frame(task, KERNEL_DATA_SELECTOR, 0, 0, 0, 0,
			    (unsigned)stack_buttom, (unsigned)stack_buttom,
			    (unsigned)stack_buttom, (unsigned)ps_run);
	ps_put_to_ready_queue(task);
	ps_add_mgr(task);
	return task->psid;
}

/* -------------------------------------------------------------------------
 * Fork helpers
 * ------------------------------------------------------------------------- */

/* Duplicate all open file descriptors from parent to child.
 * Each duplicated file gets an extra reference via fs_get_file(). */
static void ps_dup_fds(task_struct *cur, task_struct *task)
{
	int i = 0;
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

extern short pgc_entry_count[1024];

/* Map a single virtual page from the parent into the child using COW:
 * - mark the parent's page read-only so a write triggers a fault
 * - share the physical page with the child (increment reference count)
 * The page is referenced rather than copied; copy happens lazily on write. */
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

	/* Allocate a new page table entry in the child if not yet present. */
	if (!(new_ps[page_dir_offset] & PAGE_SIZE_MASK)) {
		new_ps[page_dir_offset] = mm_alloc_page_table() - KERNEL_OFFSET;
		new_ps[page_dir_offset] |= flag;
	}

	page_table = (new_ps[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET;
	cur_page_table = (cur_page_dir[page_dir_offset] & PAGE_SIZE_MASK) +
			 KERNEL_OFFSET;

	/* Mark parent's page read-only to trigger COW on next write. */
	cur_page_table[page_table_offset] &= ~PAGE_ENTRY_WRITABLE;

	phy = page_table[page_table_offset] & PAGE_SIZE_MASK;
	if (!phy) {
		idx = (PAGE_TABLE_CACHE_END - (unsigned)page_table) /
			      PAGE_SIZE -
		      1;
		pgc_entry_count[idx]++;
	}

	/* Share the physical page with the child. */
	page_table[page_table_offset] = cur_page_table[page_table_offset];
	phymm_reference_page(target_page_idx);
}

static void ps_enum_for_dup(void *aux, unsigned vir, unsigned phy)
{
	task_struct *task = aux;
	ps_dup_user_page(vir, task, PAGE_ENTRY_USER_DATA);
}

/* Duplicate the parent's user address space into the child using COW.
 * All user pages are shared read-only; writes will fault and trigger
 * actual page copies. */
static void ps_dup_user_maps(task_struct *cur, task_struct *task)
{
	unsigned int *new_ps;

	task->user.page_dir = vm_alloc(1);
	vm_dup(cur->user.vm, task->user.vm);
	new_ps = (unsigned int *)task->user.page_dir;
	/* Zero only the user portion; copy the kernel portion from the global
	 * kernel page directory so the child shares the same physical kernel
	 * page tables from birth — no per-switch sync required. */
	memset(new_ps, 0, KERNEL_PAGE_DIR_OFFSET * sizeof(unsigned int));
	mm_copy_kernel_pgd(new_ps);
	ps_enum_user_map(cur, ps_enum_for_dup, task);
}

extern void ret_from_syscall();

/* Core fork implementation.  Creates a copy of the current task:
 * - copies TSS, sets child's eax=0 (fork returns 0 in child)
 * - splits time slice between parent and child
 * - duplicates file descriptors and user address space (COW)
 * If FORK_FLAG_VFORK is set the parent blocks until the child calls exec/exit. */
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

	/* Split the remaining time slice so neither task has an unfair head-start. */
	if (cur->remain_ticks > DEFAULT_TASK_TIME_SLICE / 2)
		cur->remain_ticks = task->remain_ticks = cur->remain_ticks / 2;
	else
		task->remain_ticks = cur->remain_ticks;
	task->timeout = cur->timeout;
	task->psid = ps_id_gen();
	mutex_init(&task->fd_lock);

	/* Kernel registers: set for re-entry via ret_from_syscall. */
	task->tss.fs = task->tss.gs = task->tss.ds = task->tss.es =
		task->tss.ss = KERNEL_DATA_SELECTOR;
	task->tss.cs = KERNEL_CODE_SELECTOR;
	task->tss.eax = 0; /* child's fork() return value */
	task->tss.ebp = (char *)task_intr_frame;
	task->tss.esp = (char *)task_intr_frame;
	task->tss.esp0 = (unsigned)task + PAGE_SIZE;
	task->tss.eip = (unsigned)ret_from_syscall;
	task_intr_frame->eax = 0;

	task->parent = cur->psid;
	task->fds = vm_alloc(1);
	task->ps_list.next = task->ps_list.prev = 0;
	RB_CLEAR_NODE(&task->rb_node);
	task->sched_seq = 0;
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
	ps_put_to_ready_queue(task);
	ps_add_mgr(task);

	if (flag & FORK_FLAG_VFORK) {
		/* Block parent until child releases address space. */
		cond_init(&task->vfork_event, "vfork_event", 1);
		cond_wait(&task->vfork_event);
	}

	cur_intr_frame->eax = task->psid; /* parent's fork() return value */
	return task->psid;
}

int sys_fork()
{
	if (TestControl.verbos)
		klog("%d: fork()\n", CURRENT_TASK()->psid);
	return do_fork(0);
}

/*
 * vfork: same COW duplication as fork but the parent is suspended until the
 * child calls exec or exit, avoiding unnecessary COW faults.
 */
int sys_vfork()
{
	if (TestControl.verbos)
		klog("%d: vfork()\n", CURRENT_TASK()->psid);
	return do_fork(FORK_FLAG_VFORK);
}

/* -------------------------------------------------------------------------
 * Process exit
 * ------------------------------------------------------------------------- */

/* Release all resources owned by the current task and move it to the dying
 * queue.  Heavy resources (page_dir, command string) are left for the parent's
 * waitpid to free so this path stays interruptible. */
int sys_exit(unsigned status)
{
	task_struct *cur = CURRENT_TASK();
	int i = 0;
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
 * Miscellaneous syscalls
 * ------------------------------------------------------------------------- */

char *sys_getcwd(char *buf, unsigned size)
{
	task_struct *cur = CURRENT_TASK();
	strcpy(buf, cur->cwd[0] ? cur->cwd : "/");
	return buf;
}

/* -------------------------------------------------------------------------
 * waitpid / child reaping
 * ------------------------------------------------------------------------- */

/* Free all deferred resources of a reaped task and populate rusage if
 * requested.  Called after the task has been removed from dying_queue. */
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
	ps_remove_mgr(task);
	vm_free(task, 1);
}

/*
 * Wait for a child to finish, optionally matching a specific pid (pid != 0).
 * Loops until a child is available:
 *   1. Yield the CPU.
 *   2. Scan the dying queue for a matching child; reap and return if found.
 *   3. Scan mgr_queue for a ptrace-stopped child; report and return if found.
 *   4. No child ready: block on the wait queue and retry.
 */
int do_waitpid(unsigned pid, int *status, int options, rusage *rusage)
{
	task_struct *cur = CURRENT_TASK();
	list_entry *entry;

	for (;;) {
		task_sched();

		/* Phase 1: reap a matching child from the dying queue. */
		spinlock_lock(&dying_queue_lock);
		entry = control.dying_queue.prev;
		while (entry != &control.dying_queue) {
			task_struct *task =
				container_of(entry, task_struct, ps_list);
			entry = entry->prev;
			if (task->parent != cur->psid)
				continue;
			if (pid && pid != task->psid)
				continue;
			/* Found: remove from dying queue, then reap outside lock. */
			int ret = task->psid;
			if (status)
				*status = task->exit_status;
			list_remove_entry(&task->ps_list);
			spinlock_unlock(&dying_queue_lock);
			ps_reap_task(task, rusage);
			if (TestControl.verbos)
				klog("%d: wait(%d) = %d\n", cur->psid, pid,
				     ret);
			return ret;
		}
		spinlock_unlock(&dying_queue_lock);

		/* Phase 2: report a ptrace-stopped child (not yet reaped). */
		spinlock_lock(&mgr_queue_lock);
		entry = control.mgr_queue.prev;
		while (entry != &control.mgr_queue) {
			task_struct *task =
				container_of(entry, task_struct, ps_mgr);
			entry = entry->prev;
			if (task->parent != cur->psid)
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
			spinlock_unlock(&mgr_queue_lock);
			if (TestControl.verbos)
				klog("%d: wait(%d) = %d\n", cur->psid, pid,
				     ret);
			return ret;
		}
		spinlock_unlock(&mgr_queue_lock);

		/* No child ready: block until a child dies and notifies us. */
		ps_put_to_wait_queue(cur);
	}
}

int sys_waitpid(unsigned pid, int *status, int options)
{
	return do_waitpid(pid, status, options, NULL);
}

/* -------------------------------------------------------------------------
 * TSS management
 * The TSS (Task State Segment) holds the kernel ESP0 and CR3 that the CPU
 * loads on privilege-level transitions.  It must be updated on every context
 * switch so ring-3 faults land on the correct kernel stack.
 * ------------------------------------------------------------------------- */

/* Reload the global TSS with the given task's CR3 and kernel stack pointer. */
static void reset_tss(task_struct *task)
{
	tss_address->cr3 = task->cr3;
	tss_address->esp0 = task->tss.esp0;
	tss_address->iomap = (unsigned short)sizeof(tss_struct);
	tss_address->ss0 = KERNEL_DATA_SELECTOR;
	tss_address->ss = tss_address->gs = tss_address->fs = tss_address->ds =
		tss_address->es = KERNEL_DATA_SELECTOR | 0x3;
	tss_address->cs = KERNEL_CODE_SELECTOR | 0x3;
	int_update_tss((unsigned int)tss_address);
}

/* Called from syscall entry after the kernel stack pointer changes (e.g.
 * after a thread grows its kernel stack). */
void ps_update_tss(unsigned int esp0)
{
	task_struct *task = CURRENT_TASK();
	task->tss.esp0 = esp0;
	reset_tss(task);
}

/* -------------------------------------------------------------------------
 * Scheduler bootstrap and current-task accessor
 * ------------------------------------------------------------------------- */

/* Mark the bootstrap task (psid=0xffffffff) as non-schedulable and perform
 * the first context switch into the real task queue. */
void ps_kickoff()
{
	task_struct *cur = CURRENT_TASK();
	cur->psid = 0xffffffff;
	cur->ps_list.prev = cur->ps_list.next = 0;
	_ps_enabled = 1;
	task_sched();
}

/* Return a pointer to the currently running task_struct.
 * Each task occupies exactly KERNEL_TASK_SIZE pages aligned to PAGE_SIZE,
 * and the task_struct sits at the base of that allocation.  Masking the
 * stack pointer therefore gives us the struct address in O(1) without a
 * global variable. */
task_struct *CURRENT_TASK()
{
	task_struct *info = NULL;
	info = (task_struct *)(((unsigned int)&info) & PAGE_SIZE_MASK);
	return info;
}

/* -------------------------------------------------------------------------
 * User address-space management
 * ------------------------------------------------------------------------- */

static void ps_cleanup_enum_callback(void *aux, unsigned vir, unsigned phy)
{
	mm_del_dynamic_map(vir);
}

/* Unmap all user pages for task and flush the TLB. */
void ps_cleanup_all_user_map(task_struct *task)
{
	ps_enum_user_map(task, ps_cleanup_enum_callback, 0);
	RELOAD_CR3();
}

/* Walk every mapped user page in task's page directory and invoke fn(aux, vir, phy)
 * for each.  Only covers the user portion (indices 0..KERNEL_PAGE_DIR_OFFSET-1). */
void ps_enum_user_map(task_struct *task, fpuser_map_callback fn, void *aux)
{
	unsigned i = 0, j = 0;
	unsigned int *page_dir = NULL;

	if (!fn || !task->user.page_dir)
		return;

	page_dir = (unsigned int *)task->user.page_dir;
	for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
		unsigned *page_table =
			(unsigned *)(page_dir[i] & PAGE_SIZE_MASK);
		if (!page_table)
			continue;

		page_table = (unsigned *)((unsigned)page_table + KERNEL_OFFSET);
		for (j = 0; j < 1024; j++) {
			if ((page_table[j] & PAGE_SIZE_MASK) == 0)
				continue;
			unsigned vir = (i << 22) + (j << 12);
			unsigned phy = page_table[j] & PAGE_SIZE_MASK;
			fn(aux, vir, phy);
		}
	}
}

/* -------------------------------------------------------------------------
 * Context switch
 * ------------------------------------------------------------------------- */

/*
 * _task_sched — perform a voluntary or preemptive context switch.
 *
 * Steps:
 *   1. Disable interrupts (switch must be atomic).
 *   2. Pick the next runnable task.
 *   3. If next == current: re-enable interrupts and return (no-op).
 *   4. Otherwise:
 *      a. SAVE_ALL: stash all registers into current->tss; sets eip=NEXT label.
 *      b. RESTORE_ALL: load registers from next->tss into CPU.
 *      c. SET_CR3: switch address space.
 *      d. reset_tss: update hardware TSS for the new task.
 *      e. JUMP_TO_NEXT_TASK_EIP: jump to the next task's saved eip.
 *         On first run this is ps_run; on subsequent runs it is the NEXT label.
 *   5. When this task is later rescheduled execution resumes at NEXT.
 */
void _task_sched(const char *func)
{
	task_struct *task = 0;
	task_struct *current = 0;
	int intr_enabled = int_is_intr_enabled();

	sched_cal_begin();
	int_intr_disable();

	current = CURRENT_TASK();
	current->is_switching = 1;
	task = ps_get_next_task();

	if (task->psid == current->psid)
		goto SELF;

	task->status = ps_running;
	SAVE_ALL(current, NEXT);

	/* RESTORE_ALL loads all registers from task's TSS and switches ESP/EBP
	 * to the next task's stack.  We pass task->tss.eip as the output lvalue;
	 * the resulting self-assignment (task->tss.eip = task->tss.eip) is a
	 * no-op that executes before the stack pointer changes.
	 * After RESTORE_ALL, task is unreachable (old stack gone).
	 * CURRENT_TASK() derives the new task from the updated ESP, so cr3 and
	 * eip are read directly from the new task_struct — no globals needed. */
	RESTORE_ALL(task, task->tss.eip);
	current = CURRENT_TASK();
	SET_CR3(current->user.page_dir - KERNEL_OFFSET);
	reset_tss(current);
	JUMP_TO_NEXT_TASK_EIP(current->tss.eip);
	asm volatile("NEXT: nop");
SELF:
	int_intr_enable();
	current->is_switching = 0;
	sched_cal_end();
}

/* -------------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------------- */

/* Return non-zero if any task at or above the given priority level is ready. */
int ps_has_ready(int priority)
{
	int i = 0;

	if (priority >= MAX_PRIORITY || priority < 0)
		return 0;

	for (i = priority; i < MAX_PRIORITY; i++) {
		if (!RB_EMPTY_ROOT(&control.ready_queue[i]))
			return 1;
	}
	return 0;
}

/* Invoke callback for every live task (in mgr_queue order). */
void ps_enum_all(ps_enum_callback callback)
{
	list_entry *head = &control.mgr_queue;
	list_entry *node = head->prev;
	if (!callback)
		return;

	while (node != head) {
		task_struct *task =
			(task_struct *)container_of(node, task_struct, ps_mgr);
		callback(task);
		node = node->prev;
	}
}

/* -------------------------------------------------------------------------
 * System shutdown
 * ------------------------------------------------------------------------- */

static void close_fp_callback(task_struct *task)
{
	int i = 0;
	for (i = 0; i < MAX_FD; i++) {
		if (task->fds == NULL)
			continue;
		if (task->fds[i].used == 0 || task->fds[i].fp == NULL)
			continue;
		fs_put_file(task->fds[i].fp);
	}
}

/* Flush all pending I/O and unmount filesystems before power-off. */
static void system_down()
{
	klog_close();
	ps_enum_all(close_fp_callback);
	ext4_umount("/");
	block_close();
}

void reboot()
{
	DISABLE_INTR();
	system_down();
	port_write_byte(0x64, 0xfe);
}

void shutdown()
{
	const char s[] = "Shutdown";
	const char *p;
	DISABLE_INTR();
	printf("Shutting down system ...\n");
	system_down();

	/* QEMU/Bochs power-off via ISA debug-exit port. */
	printf("Power off...\n");
	for (p = s; *p != '\0'; p++)
		port_write_byte(0x8900, *p);

	/* Alternatively: -device isa-debug-exit,iobase=0xf4,iosize=0x04 */
	port_write_byte(0xf4, 0x00);

	printf("still running...\n");
	for (;;)
		;
}
