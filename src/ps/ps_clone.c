#include <ps/ps.h>
#include <int/int.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <errno.h>
#include <lib/klib.h>

#include "ps_internal.h"

#define CSIGNAL 0x000000ff
#define CLONE_VFORK 0x00004000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000

static int do_clone(unsigned long flags, unsigned long child_stack,
		    int *parent_tidptr, int *tls, int *child_tidptr)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *cur_intr_frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(intr_frame));
	task_struct *task;
	unsigned long unsupported;

	(void)tls;

	unsupported =
		flags &
		~(unsigned long)(CSIGNAL | CLONE_VFORK | CLONE_PARENT_SETTID |
				 CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID);
	if (unsupported)
		return -ENOSYS;

	if (flags & CLONE_VFORK) {
		if ((flags & CSIGNAL) != SIGCHLD)
			return -EINVAL;
		return do_vfork();
	}

	if ((flags & CSIGNAL) != SIGCHLD)
		return -EINVAL;

	task = fork_alloc_child(cur);
	if (!task)
		return -ENOMEM;

	task->user = zalloc(sizeof(user_enviroment));
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

	task->fds = vm_alloc(1);
	task->fd_cloexec = zalloc(FD_BITMAP_WORDS * sizeof(unsigned long));
	ps_dup_fds(cur, task);
	copy_page_range(cur, task);

	if (child_stack) {
		intr_frame *task_intr_frame =
			(intr_frame *)((char *)task + PAGE_SIZE -
				       sizeof(intr_frame));
		task_intr_frame->esp = (void *)child_stack;
		task->tss.esp = (unsigned)task_intr_frame;
		task->tss.ebp = (unsigned)task_intr_frame;
	}

	if ((flags & CLONE_PARENT_SETTID) && parent_tidptr)
		*parent_tidptr = task->psid;
	if ((flags & CLONE_CHILD_SETTID) && child_tidptr)
		*child_tidptr = task->psid;
	if ((flags & CLONE_CHILD_CLEARTID) && child_tidptr)
		task->clear_child_tid = child_tidptr;

	fork_enqueue(cur, task);
	cur_intr_frame->eax = task->psid;
	return task->psid;
}

int sys_clone(unsigned long flags, unsigned long child_stack,
	      int *parent_tidptr, int *tls, int *child_tidptr)
{
	return do_clone(flags, child_stack, parent_tidptr, tls, child_tidptr);
}
