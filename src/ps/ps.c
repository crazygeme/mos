#include <mount.h>
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

typedef struct _ps_control {
	list_entry ready_queue[MAX_PRIORITY];
	list_entry dying_queue;
	list_entry wait_queue;
	list_entry mgr_queue;
	task_struct *ps_dsr;
} ps_control;

static unsigned next_cr3;
static unsigned next_eip;

static ps_control control;
static spinlock_t ps_lock;
static spinlock_t psid_lock;
static spinlock_t dying_queue_lock;
static spinlock_t wait_queue_lock;
static spinlock_t mgr_queue_lock;
static spinlock_t map_lock;
static unsigned int ps_id;
static void lock_ps()
{
	spinlock_lock(&ps_lock);
}

static void unlock_ps()
{
	spinlock_unlock(&ps_lock);
}

static void lock_dying()
{
	spinlock_lock(&dying_queue_lock);
}

static void unlock_dying()
{
	spinlock_unlock(&dying_queue_lock);
}

static void lock_waiting()
{
	spinlock_lock(&wait_queue_lock);
}

static void unlock_waiting()
{
	spinlock_unlock(&wait_queue_lock);
}

static void lock_mgr()
{
	spinlock_lock(&mgr_queue_lock);
}

static void unlock_mgr()
{
	spinlock_unlock(&mgr_queue_lock);
}

static void ps_add_mgr(task_struct *task)
{
	lock_mgr();
	list_insert_tail(&control.mgr_queue, &task->ps_mgr);
	unlock_mgr();
}

static void ps_remove_mgr(task_struct *task)
{
	lock_mgr();
	list_remove_entry(&task->ps_mgr);
	unlock_mgr();
}

static void ps_notify_process(unsigned pid)
{
	task_struct *task = ps_find_process(pid);
	if (task) {
		ps_put_to_ready_queue(task);
	}
}

task_struct *ps_find_process(unsigned psid)
{
	list_entry *head = &control.mgr_queue;
	list_entry *entry;
	task_struct *task = 0;

	lock_mgr();

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
	unlock_mgr();

	return task;
}

void ps_put_to_dying_queue(task_struct *task)
{
	// Remove from whatever queue it is, then put into dying queue
	lock_dying();
	if (task->ps_list.prev && task->ps_list.next) {
		list_remove_entry(&task->ps_list);
	}
	if (task->psid != 0xffffffff)
		list_insert_tail(&control.dying_queue, &task->ps_list);

	// insert into list before put the status into ps_dying
	// because a ps_dying task will not be picked up by
	// scheduler. If this task is interrupted between set
	// status and put into list, it will not scheduled anymore
	// which means it will never go into dying_queue
	task->status = ps_dying;

	ps_notify_process(task->parent);

	unlock_dying();
}

void ps_put_to_wait_queue(task_struct *task)
{
	// Remove from whatever queue it is, then put into wait queue
	lock_waiting();
	if (task->ps_list.prev && task->ps_list.next) {
		list_remove_entry(&task->ps_list);
	}
	if (task->psid != 0xffffffff)
		list_insert_tail(&control.wait_queue, &task->ps_list);

	unlock_waiting();
}

void ps_put_to_ready_queue(task_struct *task)
{
	lock_ps();
	if (task->type == ps_dsr) {
		control.ps_dsr = task;
	} else {
		// Remove from whatever queue it is, then put into ready queue
		if (task->ps_list.prev && task->ps_list.next) {
			list_remove_entry(&task->ps_list);
		}

		if (task->psid != 0xffffffff)
			list_insert_tail(&control.ready_queue[task->priority],
					 &task->ps_list);
	}
	unlock_ps();
}

static task_struct *ps_get_available_ready_task(list_entry *ready_queue)
{
	list_entry *entry;
	task_struct *task = 0;
	entry = ready_queue->prev;
	unsigned now = time_now_ms();

	while (entry != ready_queue) {
		task = container_of(entry, task_struct, ps_list);
		if (task->status == ps_dying || task->timeout > now) {
			task = 0;
			entry = entry->prev;
			continue;
		} else {
			break;
		}
	}

	// put it to last, so it will not be picked immediately next time
	if (task) {
		list_remove_entry(&task->ps_list);
		list_insert_tail(&control.ready_queue[task->priority],
				 &task->ps_list);
	}
	return task;
}

static task_struct *ps_get_next_task()
{
	task_struct *task = 0;
	int i = MAX_PRIORITY - 1;
	list_entry *ready_queue;
	if (dsr_has_task()) {
		return control.ps_dsr;
	}

	lock_ps();

	for (; i >= 0; i--) {
		ready_queue = &control.ready_queue[i];
		if (list_is_empty(ready_queue)) {
			continue;
		}

		task = ps_get_available_ready_task(ready_queue);
		if (!task) {
			continue;
		} else {
			break;
		}
	}
	unlock_ps();
	return task;
}

static int _ps_enabled = 0;
unsigned long long task_schedule_time = 0;
unsigned task_schedule_count = 0;
static unsigned long long sched_begin = 0;
static unsigned long long sched_end = 0;

static void sched_cal_begin()
{
	sched_begin = time_now_us();
}

static void sched_cal_end()
{
	sched_end = time_now_us();
	if (sched_begin) {
		task_schedule_time += (sched_end - sched_begin);
	}
	sched_begin = sched_end = 0;
	task_schedule_count++;
}

static void ps_run()
{
	task_struct *task;
	process_fn fn;
	void *param;

	int_intr_enable();
	_ps_enabled = 1;
	task = CURRENT_TASK();
	task->status = ps_running;
	task->is_switching = 0;
	fn = task->fn;
	if (fn) {
		fn(0);
	}

	task->status = ps_dying;
	// put to dying_queue
	ps_put_to_dying_queue(task);
	ps_remove_mgr(task);
	ps_notify_process(task->parent);
	task_sched();
	// vm_free(task, KERNEL_TASK_SIZE);
}

static int debug = 0;
static tss_struct *tss_address = 0;

void ps_init()
{
	int i = 0;
	list_init(&control.dying_queue);
	list_init(&control.wait_queue);
	list_init(&control.mgr_queue);
	for (i = 0; i < MAX_PRIORITY; i++) {
		list_init(&control.ready_queue[i]);
	}
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
	debug = 0;
	tss_address = kmalloc(sizeof(tss_struct));
	int_update_tss((unsigned int)tss_address);
}

int ps_enabled()
{
	return _ps_enabled;
}

static void *psid_map[PSID_MAP_SIZE] = { 0 };
static inline unsigned int __bsr(unsigned int value)
{
	int i;
	for (i = 0; i < 32; i++) {
		if ((value % 2) == 0)
			return i;
		value >>= 1;
	}
}

static int ps_id_find(int index, unsigned *page)
{
	int i = 0;
	int begin = index * PAGE_SIZE * 8;
	int off;
	for (i = 0; i < (PAGE_SIZE / sizeof(*page)); i++) {
		if (page[i] == (unsigned)-1)
			continue;
		off = __bsr(page[i]);
		return begin + i * 32 + off;
	}
}

static void ps_id_mark(int psid, int used)
{
	int cnt_per_pg = PAGE_SIZE * 8;
	int pg_idx = psid / cnt_per_pg;
	int pg_off = psid % cnt_per_pg;
	int bit_idx = pg_off / 32;
	int bit_off = pg_off % 32;
	unsigned mask = (unsigned)1 << (bit_off);
	unsigned *page = (unsigned *)psid_map[pg_idx];
	if (used)
		page[bit_idx] |= mask;
	else
		page[bit_idx] &= ~mask;
}

static unsigned do_ps_id_gen()
{
	int ret = -1;
	int i = 0;
	void *page = NULL;

	spinlock_lock(&psid_lock);

	for (i = 0; i < PSID_MAP_SIZE; i++) {
		if (psid_map[i] != NULL) {
			ret = ps_id_find(i, psid_map[i]);
			if (ret >= 0)
				break;
		} else {
			break;
		}
	}

	if (ret < 0 && i < PSID_MAP_SIZE) {
		psid_map[i] = vm_alloc(1);
		ret = ps_id_find(i, psid_map[i]);
	}

	if (ret < 0) {
		// too many processes!!
		printk("Too many processes");
		DIE();
	}

	ps_id_mark(ret, 1);

	spinlock_unlock(&psid_lock);
	return (unsigned)ret;
}
#ifdef PSID_ALLOC
static unsigned last_gen_pid = (unsigned)-1;
#else
static unsigned last_gen_pid = (unsigned)0;
#endif

unsigned ps_id_gen()
{
#ifdef PSID_ALLOC
	unsigned ret = do_ps_id_gen();
	unsigned ret2;
	unsigned id;
	if (__sync_add_and_fetch(&last_gen_pid, 0) == ret) {
		ret2 = do_ps_id_gen();
		ps_id_free(ret);
		__sync_lock_test_and_set(&last_gen_pid, ret2);
		id = ret2;
	} else {
		__sync_lock_test_and_set(&last_gen_pid, ret);
		id = ret;
	}
	return id;
#else
	return __sync_fetch_and_add(&last_gen_pid, 1);
#endif
}

void ps_id_free(unsigned psid)
{
#ifdef PSID_ALLOC
	spinlock_lock(&psid_lock);
	ps_id_mark(psid, 0);
	spinlock_unlock(&psid_lock);
#endif
}

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

// FIXME
// now we ignore priority
unsigned ps_create(process_fn fn, int priority, ps_type type)
{
	unsigned int stack_buttom;
	int i = 0;
	int size = 0;
	task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);

	if (priority >= MAX_PRIORITY) {
		vm_free(task, 1);
		return 0xffffffff;
	}

	memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);

	size = sizeof(*task);

	task->user.reserve = (unsigned)vm_alloc(1);
	task->user.page_dir = (unsigned int)vm_alloc(1);
	task->command = vm_alloc(1);
	task->umask = 0;
	strcpy(task->command, "system");
	// 0 ~ KERNEL_PAGE_DIR_OFFSET-1 are dynamic, KERNEL_PAGE_DIR_OFFSET ~ 1023 are shared
	memset(task->user.page_dir, 0, PAGE_SIZE);

	stack_buttom = (unsigned int)task + KERNEL_TASK_SIZE * PAGE_SIZE - 4;
	LOAD_CR3(task->cr3);
	task->ps_list.prev = task->ps_list.next = 0;
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
	// strcpy(task->cwd, "\0");

	mutex_init(&task->fd_lock);
	task->magic = 0xdeadbeef;

	ps_setup_task_frame(task, KERNEL_DATA_SELECTOR, 0, 0, 0, 0,
			    (unsigned)stack_buttom, (unsigned)stack_buttom,
			    (unsigned)stack_buttom, (unsigned)ps_run);
	ps_put_to_ready_queue(task);
	ps_add_mgr(task);
	return task->psid;
}

static void ps_dup_fds(task_struct *cur, task_struct *task)
{
	unsigned fd;
	int i = 0;
	memset(task->fds, 0, PAGE_SIZE);
	mutex_lock(&cur->fd_lock);
	for (i = 0; i < MAX_FD; i++) {
		if (!cur->fds[i].used)
			continue;

		task->fds[i] = cur->fds[i];
		fs_refrence(cur->fds[i].fp);
	}
	mutex_unlock(&cur->fd_lock);
}

extern short pgc_entry_count[1024];
static void ps_dup_user_page(unsigned vir, task_struct *task, unsigned flag)
{
	unsigned int *new_ps = (unsigned int *)task->user.page_dir;
	unsigned target_page_idx = mm_get_attached_page_index(vir);
	unsigned target_page_addr = target_page_idx * PAGE_SIZE;
	unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
	unsigned int page_table_offset = ADDR_TO_PET_OFFSET(vir);
	unsigned int *cur_page_table;
	unsigned int *cur_page_dir = mm_get_pagedir();
	task_struct *cur = CURRENT_TASK();
	unsigned int *page_table;
	unsigned int tmp, phy;
	int i = 0, idx;

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
	task_struct *task = aux;
	ps_dup_user_page(vir, task, PAGE_ENTRY_USER_DATA);
}

static void ps_dup_user_maps(task_struct *cur, task_struct *task)
{
	unsigned int *per_ps = 0;
	unsigned int *new_ps = 0;
	list_entry *entry = 0;
	int i = 0;
	unsigned int cr3;

	LOAD_CR3(cr3);
	per_ps = (unsigned int *)(cr3 + KERNEL_OFFSET);
	task->user.page_dir = vm_alloc(1);

	vm_dup(cur->user.vm, task->user.vm);

	new_ps = (unsigned int *)task->user.page_dir;
	memset((char *)new_ps, 0, PAGE_SIZE);
	ps_enum_user_map(cur, ps_enum_for_dup, task);
}

extern void ret_from_syscall();
int do_fork(unsigned flag)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *task = vm_alloc(KERNEL_TASK_SIZE);
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	intr_frame *task_intr_frame =
		(intr_frame *)((char *)task + PAGE_SIZE - sizeof(intr_frame));

	// memcpy(task, cur, PAGE_SIZE);
	*task = *cur;
	*task_intr_frame = *cur_intr_frame;
	if (cur->remain_ticks > DEFAULT_TASK_TIME_SLICE / 2)
		cur->remain_ticks = task->remain_ticks = cur->remain_ticks / 2;
	else
		task->remain_ticks = cur->remain_ticks;
	task->timeout = cur->timeout;
	task->psid = ps_id_gen();
	mutex_init(&task->fd_lock);

	// for kernel part
	// when return from intr, those will restored by user mode value
	task->tss.fs = task->tss.gs = task->tss.ds = task->tss.es =
		task->tss.ss = KERNEL_DATA_SELECTOR;
	task->tss.cs = KERNEL_CODE_SELECTOR;
	// those are for new task
	task->tss.eax = 0;
	task->tss.ebp = (char *)task_intr_frame;
	task->tss.esp = (char *)task_intr_frame;
	task->tss.esp0 = (unsigned)task + PAGE_SIZE;
	task->tss.eip = (unsigned)ret_from_syscall;
	task_intr_frame->eax = 0;
	task->parent = cur->psid;
	task->fds = vm_alloc(1); // kmalloc(MAX_FD*sizeof(fd_type));
	task->ps_list.next = task->ps_list.prev = 0;
	task->user.vm = vm_create();
	task->user.reserve = (unsigned)vm_alloc(1);
	task->command = vm_alloc(1);
	task->umask = cur->umask;
	task->priority = cur->priority;
	strcpy(task->command, cur->command);
	task->cwd = name_get();
	task->fork_flag = flag;
	task->root = cur->root;
	mount_ref(task->root);
	memset(task->cwd, 0, MAX_PATH);
	strcpy(task->cwd, cur->cwd);
	ps_dup_fds(cur, task);
	ps_dup_user_maps(cur, task);
	ps_put_to_ready_queue(task);
	ps_add_mgr(task);
	if (flag & FORK_FLAG_VFORK) {
		cond_init(&task->vfork_event, "vfork_event", 1);
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

/*
 * currently we still duplication address space
 * it's fast enough because of COW
 */
int sys_vfork()
{
	if (TestControl.verbos)
		klog("%d: vfork()\n", CURRENT_TASK()->psid);

	return do_fork(FORK_FLAG_VFORK);
}

int sys_exit(unsigned status)
{
	task_struct *cur = CURRENT_TASK();
	int i = 0;
	cur->exit_status = status;
	if (TestControl.verbos)
		klog("%d: exit(%d)\n", cur->psid, status);

	if (cur->fork_flag & FORK_FLAG_VFORK) {
		cond_notify(&cur->vfork_event);
	}

	if (cur->user.vm) {
		vm_destroy(cur->user.vm);
		cur->user.vm = 0;
	}

	// close all fds
	for (i = 0; i < MAX_FD; i++) {
		if (cur->fds[i].used) {
			fs_close(i);
		}
	}

	vm_free(cur->fds, 1);
	cur->fds = NULL;

	// free all physical memory
	ps_cleanup_all_user_map(cur);

	if (cur->psid == 0) {
		printk("fatal error! process 0 exit\n");
		DIE();
	}

	if (cur->psid == 1) {
		reboot();
	}
	// things like cur->user.page_dir have to to freed in
	// cleanup stage (polled by waitpid) because this sys
	// call can be interrupted

	// FIXME
	// modify all child->parent into cur->parent
	ps_put_to_dying_queue(cur);

	task_sched();

	return 0;
}

char *sys_getcwd(char *buf, unsigned size)
{
	task_struct *cur = CURRENT_TASK();

	if (!cur->cwd[0])
		strcpy(buf, "/");
	else
		strcpy(buf, cur->cwd);

	return buf;
}

int sys_waitpid(unsigned pid, int *status, int options)
{
	list_entry *entry = 0;
	task_struct *cur = CURRENT_TASK();
	int ret = 0;
	int can_return = 0;
	if (TestControl.verbos) {
		klog("%d: wait(%d)\n", CURRENT_TASK()->psid, pid);
	}

	do {
		task_sched();
		lock_dying();
		entry = control.dying_queue.prev;
		while (entry != &control.dying_queue) {
			task_struct *task =
				container_of(entry, task_struct, ps_list);
			int match = 0;
			if (task->parent == cur->psid) {
				if (pid && (pid == task->psid)) {
					match = 1;
				} else if (!pid) {
					match = 1;
				}
			}

			if (match) {
				ret = task->psid;
				if (status) {
					*status = task->exit_status;
				}
				list_remove_entry(entry);
				if (task->user.reserve) {
					vm_free(task->user.reserve, 1);
					task->user.reserve = 0;
				}

				if (task->command) {
					vm_free(task->command, 1);
					task->command = 0;
				}

				if (task->cwd) {
					name_put(task->cwd);
					task->cwd = 0;
				}

				if (task->user.page_dir) {
					vm_free(task->user.page_dir, 1);
					task->user.page_dir = 0;
				}

				if (task->root) {
					mount_deref(task->root);
				}

				ps_remove_mgr(task);
				vm_free(task, 1);
				can_return = 1;

				break;
			}

			entry = entry->prev;
		}

		if (!can_return) {
			ps_put_to_wait_queue(cur);
		}
		unlock_dying();
	} while (can_return == 0);
	ps_id_free(ret);
	return ret;
}

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

void ps_update_tss(unsigned int esp0)
{
	task_struct *task = CURRENT_TASK();
	task->tss.esp0 = esp0;
	reset_tss(task);
}

void ps_kickoff()
{
	task_struct *cur = CURRENT_TASK();
	cur->psid = 0xffffffff;
	cur->ps_list.prev = cur->ps_list.next = 0;
	_ps_enabled = 1;
	task_sched();
	return;
}

task_struct *CURRENT_TASK()
{
	unsigned int esp;

	LOAD_ESP(esp);
	return (task_struct *)((unsigned int)esp & PAGE_SIZE_MASK);
}

static void ps_cleanup_enum_callback(void *aux, unsigned vir, unsigned phy)
{
	mm_del_dynamic_map(vir);
}

void ps_cleanup_all_user_map(task_struct *task)
{
	ps_enum_user_map(task, ps_cleanup_enum_callback, 0);
	RELOAD_CR3();
}

void ps_enum_user_map(task_struct *task, fpuser_map_callback fn, void *aux)
{
	unsigned i = 0, j = 0;
	unsigned int *page_dir = NULL;

	if (!fn) {
		return;
	}

	if (!task->user.page_dir) {
		return;
	}

	page_dir = (unsigned int *)task->user.page_dir;
	for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
		unsigned *page_table =
			(unsigned *)(page_dir[i] & PAGE_SIZE_MASK);
		if (!page_table) {
			continue;
		}

		page_table = (unsigned *)((unsigned)page_table + KERNEL_OFFSET);
		for (j = 0; j < 1024; j++) {
			if ((page_table[j] & PAGE_SIZE_MASK) == 0) {
				continue;
			}

			unsigned vir = (i << 22) + (j << 12);
			unsigned phy = page_table[j] & PAGE_SIZE_MASK;
			fn(aux, vir, phy);
		}
	}
}

static void ps_save_kernel_map(task_struct *task)
{
	if (task->user.page_dir) {
		unsigned int cr3;
		unsigned int *in_use;
		unsigned int *per_ps = (unsigned int *)task->user.page_dir;
		void *src;
		void *dst;

		LOAD_CR3(cr3);
		in_use = (unsigned int *)(cr3 + KERNEL_OFFSET);
		src = &in_use[KERNEL_PAGE_DIR_OFFSET];
		dst = &per_ps[KERNEL_PAGE_DIR_OFFSET];
		memcpy(dst, src,
		       (1024 - KERNEL_PAGE_DIR_OFFSET) * sizeof(unsigned));
	}
}

void _task_sched(const char *func)
{
	task_struct *task = 0;
	task_struct *current = 0;
	unsigned pdr;
	int i = 0;
	int intr_enabled = int_is_intr_enabled();

	sched_cal_begin();

	int_intr_disable();

	current = CURRENT_TASK();
	current->is_switching = 1;
	task = ps_get_next_task();

	if (task->psid == current->psid) {
		goto SELF;
	}

	task->status = ps_running;
	next_cr3 = task->user.page_dir - KERNEL_OFFSET;
	// save page dir entry for kernel space
	ps_save_kernel_map(task);

	SAVE_ALL(current, NEXT);

	RESTORE_ALL(task, next_eip);
	SET_CR3(next_cr3);
	current = CURRENT_TASK();
	reset_tss(current);
	JUMP_TO_NEXT_TASK_EIP(next_eip);
	asm volatile("NEXT: nop");
SELF:
	int_intr_enable();
	current->is_switching = 0;
	sched_cal_end();
	return;
}

int ps_has_ready(int priority)
{
	int i = 0;

	if (priority >= MAX_PRIORITY || priority < 0) {
		return 0;
	}

	for (i = priority; i < MAX_PRIORITY; i++) {
		list_entry *head = &(control.ready_queue[i]);
		if (!list_is_empty(head))
			return 1;
	}

	return 0;
}

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

static void close_fp_callback(task_struct *task)
{
	int i = 0;
	for (i = 0; i < MAX_FD; i++) {
		if (task->fds == NULL)
			continue;

		if (task->fds[i].used == 0)
			continue;

		if (task->fds[i].fp == NULL)
			continue;

		fs_destroy(task->fds[i].fp);
	}
}

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
	write_port(0x64, 0xfe);
}

void shutdown()
{
	const char s[] = "Shutdown";
	const char *p;
	DISABLE_INTR();
	printf("Shutting down system ...\n");
	system_down();

	/* 	This is a special power-off sequence supported by Bochs and
          QEMU, but not by physical hardware. */
	printf("Power off...\n");
	for (p = s; *p != '\0'; p++)
		write_port(0x8900, *p);

	/*  In newer versions of QEMU, you can pass
        -device isa-debug-exit,iobase=0xf4,iosize=0x04
        on the command-line, and do: */
	write_port(0xf4, 0x00);

	/* None of those works... */
	printf("still running...\n");
	for (;;)
		;
}