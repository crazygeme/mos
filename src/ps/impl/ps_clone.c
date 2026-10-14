#include <ps/ps.h>
#include <int/int.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <errno.h>
#include <lib/klib.h>

#include "ps_internal.h"

#include <ps/clone.h>
#include <ps/namespace.h>

static void clone_prepare_thread_tls(task_struct *cur, task_struct *task,
				     unsigned short parent_gs)
{
	unsigned entry;

	memset(task->execution->tls_desc, 0, sizeof(task->execution->tls_desc));

	/*
	 * A CLONE_THREAD child should not inherit every historical GDT TLS slot
	 * the parent touched, but it must still inherit the selector that is
	 * live in the parent's %gs when clone() is issued if CLONE_SETTLS is
	 * not used. Otherwise the copied child interrupt frame still returns
	 * with (for example) gs=0x33 while GDT slot 6 has just been cleared,
	 * and intr_exit faults on "pop %gs" with #GP(error_code=0x30).
	 */
	if ((parent_gs & 0x4) != 0 || parent_gs == 0)
		return;

	entry = parent_gs >> 3;
	if (entry < GDT_ENTRY_TLS_MIN || entry > GDT_ENTRY_TLS_MAX)
		return;

	task->execution->tls_desc[entry - GDT_ENTRY_TLS_MIN] =
		cur->execution->tls_desc[entry - GDT_ENTRY_TLS_MIN];
}

static int do_clone(unsigned long flags, unsigned long child_stack,
		    int *parent_tidptr, int *tls, int *child_tidptr)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
			       sizeof(intr_frame));
	task_struct *task;
	unsigned long unsupported;
	int share_vm = !!(flags & CLONE_VM);
	int thread_group = !!(flags & CLONE_THREAD);
	int irq;
	unsigned exit_signal = flags & CSIGNAL;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("clone(flags=%x, child_stack=%x, ptid=%x, tls=%x, ctid=%x)\n",
		     (unsigned)(uintptr_t)flags,
		     (unsigned)(uintptr_t)child_stack,
		     (unsigned)(uintptr_t)parent_tidptr,
		     (unsigned)(uintptr_t)tls,
		     (unsigned)(uintptr_t)child_tidptr);

	unsupported =
		flags &
		~(unsigned long)(CSIGNAL | CLONE_VFORK | CLONE_PARENT_SETTID |
				 CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID |
				 CLONE_VM | CLONE_FS | CLONE_FILES |
				 CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS |
				 CLONE_SYSVSEM | CLONE_DETACHED | CLONE_PARENT |
				 CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
				 CLONE_NEWPID | CLONE_NEWCGROUP);
	if (unsupported)
		return (unsupported & CLONE_NAMESPACE_FLAGS) ? -EINVAL :
							       -ENOSYS;

	if ((flags & CLONE_NAMESPACE_FLAGS) && !ps_has_capability(cur, 21))
		return -EPERM;
	if ((flags & CLONE_NEWNS) && (flags & CLONE_FS))
		return -EINVAL;
	if ((flags & CLONE_NEWPID) && (flags & CLONE_THREAD))
		return -EINVAL;

	if ((flags & CLONE_SIGHAND) && !share_vm)
		return -EINVAL;
	if (thread_group && !(flags & CLONE_SIGHAND))
		return -EINVAL;

	if ((flags & CLONE_PARENT) && !thread_group &&
	    ps_pid_visible(cur->thread->tgid) == 1)
		return -EINVAL;
	if (flags & CLONE_VFORK) {
		if (flags & CLONE_PARENT)
			return -EINVAL;
		if ((flags & CSIGNAL) != SIGCHLD)
			return -EINVAL;
		return do_vfork(child_stack, flags);
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

	int clone_ret = ps_clone_resources(cur, task, flags);
	if (clone_ret) {
		fork_abort_child(task);
		return clone_ret;
	}
	if (fork_set_meta(cur, task, thread_group ? FORK_FLAG_THREAD : 0)) {
		fork_abort_child(task);
		return -ENOMEM;
	}

	if (thread_group) {
		/*
		 * A new thread gets its live TLS state from CLONE_SETTLS (or the
		 * current %gs/LDT state), not by reusing every occupied GDT TLS
		 * slot the parent happened to have touched earlier. Inheriting the
		 * parent's tls_desc[] verbatim makes set_thread_area(entry=-1)
		 * think the child has no free slots and return -ESRCH.
		 */
		clone_prepare_thread_tls(cur, task, cur_intr_frame->gs);
	}

	task->life->ppid = (thread_group || (flags & CLONE_PARENT)) ?
				   cur->life->ppid :
				   cur->thread->tgid;
	if (thread_group || (flags & CLONE_PARENT))
		task->life->creator = NULL;
	task->thread->tgid = thread_group ? cur->thread->tgid :
					    task->life->psid;
	task->life->exit_signal = exit_signal;

	if (child_stack) {
		intr_frame *task_intr_frame =
			(intr_frame *)((char *)task + KERNEL_TASK_BYTES -
				       sizeof(intr_frame));
		task_intr_frame->esp = (void *)child_stack;
	}

	if ((flags & CLONE_SETTLS) && tls) {
		int rc = ps_set_clone_tls_for(task, tls, cur_intr_frame);

		if (rc != 0) {
			fork_abort_child(task);
			return rc;
		}
	}

	if ((flags & CLONE_PARENT_SETTID) && parent_tidptr)
		*parent_tidptr = ps_pid_visible(task->life->psid);
	int child_tid = ps_pid_for(task, task->life->psid);
	if ((flags & CLONE_CHILD_SETTID) && child_tidptr) {
		if (share_vm)
			*child_tidptr = child_tid;
		else {
			int rc = ps_write_process_memory(task, child_tidptr,
							 &child_tid,
							 sizeof(child_tid));

			if (rc != 0) {
				fork_abort_child(task);
				return rc;
			}
		}
	}
	if ((flags & CLONE_CHILD_CLEARTID) && child_tidptr)
		task->execution->clear_child_tid = child_tidptr;

	if (!thread_group)
		ps_enqueue_child_first(cur, task);
	else {
		spinlock_lock(&ps_lock, &irq);
		ps_put_to_ready_queue_unsafe(task);
		ps_add_mgr_unsafe(task);
		spinlock_unlock(&ps_lock, irq);
	}
	cur_intr_frame->eax = ps_pid_visible(task->life->psid);
	/*
	 * Process-style clone() is used by glibc/NPTL for fork-like children
	 * with CLONE_CHILD_SETTID/CLEARTID. Yield once after enqueue so the
	 * newborn child can run its fork return path before the parent races
	 * ahead and signals it.
	 */
	if (!thread_group)
		task_sched();
	return ps_pid_visible(task->life->psid);
}

int sys_clone(unsigned long flags, unsigned long child_stack,
	      int *parent_tidptr, int *tls, int *child_tidptr)
{
	return do_clone(flags, child_stack, parent_tidptr, tls, child_tidptr);
}
