#include "hw/cpu.h"
#include <ps/ps.h>
#include <int/int.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <errno.h>
#include <lib/klib.h>

#include "ps_internal.h"

#define CSIGNAL 0x000000ff
#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000
#define CLONE_SETTLS 0x00080000
#define CLONE_VFORK 0x00004000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000
#define CLONE_SYSVSEM 0x00040000
#define CLONE_DETACHED 0x00400000

static void clone_enqueue_process_child(task_struct *cur, task_struct *task)
{
	int irq;

	spinlock_lock(&ps_lock, &irq);
	cur->nchildren++;
	ps_put_to_ready_queue_unsafe(task);
	/*
	 * Process-style clone() should let the newborn run its fork return path
	 * before the parent resumes. Move it to the front of its ready queue for
	 * this first scheduling decision.
	 */
	list_remove_entry(&task->ps_list);
	list_insert_head(
		&control.cpu[cpu_current_id()].ready_queue[task->priority],
		&task->ps_list);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

static int do_clone(unsigned long flags, unsigned long child_stack,
		    int *parent_tidptr, int *tls, int *child_tidptr)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	task_struct *task;
	unsigned long unsupported;
	int share_vm = !!(flags & CLONE_VM);
	int thread_group = !!(flags & CLONE_THREAD);
	int irq;
	unsigned exit_signal = flags & CSIGNAL;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("clone(flags=%x, child_stack=%x, ptid=%x, tls=%x, ctid=%x)\n",
		     (unsigned)flags, (unsigned)child_stack,
		     (unsigned)parent_tidptr, (unsigned)tls,
		     (unsigned)child_tidptr);

	unsupported =
		flags &
		~(unsigned long)(CSIGNAL | CLONE_VFORK | CLONE_PARENT_SETTID |
				 CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID |
				 CLONE_VM | CLONE_FS | CLONE_FILES |
				 CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS |
				 CLONE_SYSVSEM | CLONE_DETACHED);
	if (unsupported)
		return -ENOSYS;

	if ((flags & CLONE_SIGHAND) && !share_vm)
		return -EINVAL;
	if (thread_group && !(flags & CLONE_SIGHAND))
		return -EINVAL;

	if (flags & CLONE_VFORK) {
		if ((flags & CSIGNAL) != SIGCHLD)
			return -EINVAL;
		return do_vfork();
	}

	if (thread_group) {
		if ((flags & CSIGNAL) != 0)
			return -EINVAL;
	} else if (!share_vm && exit_signal != SIGCHLD) {
		return -EINVAL;
	}

	task = fork_alloc_child(cur);
	if (!task)
		return -ENOMEM;

	task->user = ps_alloc_user_env();
	if (!task->user)
		return -ENOMEM;

	if (share_vm) {
		task->user->page_dir = cur->user->page_dir;
		task->user->vm = cur->user->vm;
	} else {
		task->user->vm = vm_create();
		task->user->page_dir = vm_alloc(1);
		mm_init_process_page_dir(task->user->page_dir);
	}
	fork_dup_user_env(cur, task);
	if (share_vm)
		ps_share_heap_state(task->user, cur->user);
	fork_dup_signal(cur, task);
	if (fork_dup_io(cur, task) != 0)
		return -ENOMEM;
	fork_set_meta(cur, task, share_vm ? FORK_FLAG_SHARE_VM : 0);
	if (share_vm)
		task->fork_flag |= FORK_FLAG_SHARE_VM;
	if (thread_group)
		task->fork_flag |= FORK_FLAG_THREAD;

	if (thread_group) {
		/*
		 * A new thread gets its live TLS state from CLONE_SETTLS (or the
		 * current %gs/LDT state), not by reusing every occupied GDT TLS
		 * slot the parent happened to have touched earlier. Inheriting the
		 * parent's tls_desc[] verbatim makes set_thread_area(entry=-1)
		 * think the child has no free slots and return -ESRCH.
		 */
		memset(task->user->tls_desc, 0, sizeof(task->user->tls_desc));
	}

	task->fds = vm_alloc(1);
	task->fd_cloexec = zalloc(FD_BITMAP_WORDS * sizeof(unsigned long));
	ps_dup_fds(cur, task);
	if (!share_vm)
		copy_page_range(cur, task);

	task->ppid = thread_group ? cur->ppid : cur->psid;
	task->tgid = thread_group ? cur->tgid : task->psid;
	task->exit_signal = exit_signal;

	if (child_stack) {
		intr_frame *task_intr_frame =
			(intr_frame *)((char *)task + PAGE_SIZE -
				       sizeof(intr_frame));
		task_intr_frame->esp = (void *)child_stack;
		task->tss.esp = (unsigned)task_intr_frame;
		task->tss.ebp = (unsigned)task_intr_frame;
	}

	if ((flags & CLONE_SETTLS) && tls) {
		int rc = ps_set_clone_tls_for(task, tls, cur_intr_frame->gs);

		if (rc != 0)
			return rc;
	}

	if ((flags & CLONE_PARENT_SETTID) && parent_tidptr)
		*parent_tidptr = task->psid;
	if ((flags & CLONE_CHILD_SETTID) && child_tidptr) {
		if (share_vm)
			*child_tidptr = task->psid;
		else {
			int rc = ps_write_process_memory(task, child_tidptr,
							 &task->psid,
							 sizeof(task->psid));

			if (rc != 0)
				return rc;
		}
	}
	if ((flags & CLONE_CHILD_CLEARTID) && child_tidptr)
		task->clear_child_tid = child_tidptr;

	if (!thread_group)
		clone_enqueue_process_child(cur, task);
	else {
		spinlock_lock(&ps_lock, &irq);
		ps_put_to_ready_queue_unsafe(task);
		ps_add_mgr_unsafe(task);
		spinlock_unlock(&ps_lock, irq);
	}
	cur_intr_frame->eax = task->psid;
	/*
	 * Process-style clone() is used by glibc/NPTL for fork-like children
	 * with CLONE_CHILD_SETTID/CLEARTID. Yield once after enqueue so the
	 * newborn child can run its fork return path before the parent races
	 * ahead and signals it.
	 */
	if (!thread_group && ncpus > 1)
		task_sched();
	return task->psid;
}

int sys_clone(unsigned long flags, unsigned long child_stack,
	      int *parent_tidptr, int *tls, int *child_tidptr)
{
	return do_clone(flags, child_stack, parent_tidptr, tls, child_tidptr);
}
