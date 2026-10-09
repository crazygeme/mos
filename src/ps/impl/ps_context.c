/* Each clone flag selects one independently reference-counted resource. */
#include <ps/ps.h>
#include <ps/clone.h>
#include <lib/klib.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <errno.h>
#include "ps_internal.h"

static void release_fs(ref_count_t *ref)
{
	task_fs *fs = container_of(ref, task_fs, ref);
	if (fs->cwd)
		name_put(fs->cwd);
	if (fs->root_path)
		name_put(fs->root_path);
	if (fs->root)
		sb_put(fs->root);
	kfree(fs);
}

static task_fs *alloc_fs(task_fs *old)
{
	task_fs *fs = zalloc(sizeof(*fs));
	if (!fs)
		return NULL;
	ref_count_init(&fs->ref, release_fs);
	rmutex_init(&fs->lock);
	fs->cwd = name_get();
	fs->root_path = name_get();
	if (!fs->cwd || !fs->root_path) {
		ref_count_put(fs);
		return NULL;
	}
	fs->cwd[0] = '\0';
	strcpy(fs->root_path, "/");
	if (old) {
		LOCK_GUARD(&old->lock);
		strcpy(fs->cwd, old->cwd);
		strcpy(fs->root_path, old->root_path);
		fs->root = old->root;
		fs->umask = old->umask;
		if (fs->root)
			sb_get(fs->root);
	}
	return fs;
}

void ps_free_signal_queue(list_entry *queue)
{
	while (!list_is_empty(queue))
		kfree(container_of(list_remove_head(queue), signal_queue_entry,
				   list));
}

static void release_thread_group(ref_count_t *ref)
{
	task_thread *group = container_of(ref, task_thread, ref);
	ps_alarm_release_thread_group(group);
	ps_free_signal_queue(&group->pending_queue);
	kfree(group);
}

static task_thread *alloc_thread_group(task_thread *old)
{
	task_thread *group = zalloc(sizeof(*group));
	if (!group)
		return NULL;
	ref_count_init(&group->ref, release_thread_group);
	RB_CLEAR_NODE(&group->alarm_rb);
	list_init(&group->pending_queue);
	if (old) {
		LOCK_GUARD(&ps_lock);
		group->group_id = old->group_id;
		group->session_id = old->session_id;
		memcpy(group->rlimits, old->rlimits, sizeof(group->rlimits));
	} else {
		for (unsigned i = 0; i < RLIM_NLIMITS; i++)
			group->rlimits[i] =
				(rlimit_t){ RLIM_INFINITY, RLIM_INFINITY };
	}
	return group;
}

static void release_sighand(ref_count_t *ref)
{
	kfree(container_of(ref, signal_handlers, ref));
}

signal_handlers *ps_copy_sighand(signal_handlers *old)
{
	signal_handlers *handlers = zalloc(sizeof(*handlers));
	if (!handlers)
		return NULL;
	ref_count_init(&handlers->ref, release_sighand);
	if (old) {
		LOCK_GUARD(&ps_lock);
		memcpy(handlers->actions, old->actions,
		       sizeof(handlers->actions));
	}
	return handlers;
}

int ps_init_private(task_struct *task)
{
	task->sched = zalloc(sizeof(*task->sched));
	task->wait = zalloc(sizeof(*task->wait));
	task->life = zalloc(sizeof(*task->life));
	task->execution = zalloc(sizeof(*task->execution));
	task->credentials = zalloc(sizeof(*task->credentials));
	task->signal = zalloc(sizeof(*task->signal));
	if (!task->sched || !task->wait || !task->life || !task->execution ||
	    !task->credentials || !task->signal)
		return -ENOMEM;
	task->sched->task = task;
	task->wait->task = task;
	task->life->psid = (unsigned)-1;
	list_init(&task->sched->ps_list);
	list_init(&task->life->dying_queue);
	list_init(&task->wait->io_files);
	list_init(&task->signal->pending_queue);
	RB_CLEAR_NODE(&task->sched->mgr_rb);
	RB_CLEAR_NODE(&task->wait->timer_rb);
	task->magic = 0xdeadbeef;
	return 0;
}

void ps_free_task(task_struct *task)
{
	ps_put_fds(task);
	ps_put_resources(task);
	if (task->execution)
		kfree(task->execution->io_bitmap);
	kfree(task->execution);
	kfree(task->credentials);
	kfree(task->signal);
	kfree(task->sched);
	kfree(task->wait);
	kfree(task->life);
	kfree(task->stats);
	vm_free((vaddr_t)task, KERNEL_TASK_SIZE);
}

int ps_init_resources(task_struct *task)
{
	if (ps_init_private(task))
		return -ENOMEM;
	task->fs = alloc_fs(NULL);
	task->thread = alloc_thread_group(NULL);
	task->sighand = ps_copy_sighand(NULL);
	list_init(&task->signal->pending_queue);
	list_init(&task->wait->io_files);
	if (!task->execution || !task->fs || !task->thread || !task->sighand) {
		ps_put_resources(task);
		return -ENOMEM;
	}
	return 0;
}

void ps_put_resources(task_struct *task)
{
	if (task->memory) {
		ref_count_put(task->memory);
		task->memory = NULL;
	}
	if (task->fs)
		ref_count_put(task->fs);
	if (task->thread)
		ref_count_put(task->thread);
	if (task->sighand)
		ref_count_put(task->sighand);
	task->fs = NULL;
	task->thread = NULL;
	task->sighand = NULL;
	if (task->signal && task->signal->pending_queue.next)
		ps_free_signal_queue(&task->signal->pending_queue);
}

int ps_clone_resources(task_struct *parent, task_struct *child,
		       unsigned long flags)
{
	if (flags & CLONE_VM) {
		child->memory = ref_count_get(parent->memory);
	} else {
		child->memory = vm_create();
		if (!child->memory)
			return -ENOMEM;
		child->memory->page_dir = vm_alloc(1);
		if (!child->memory->page_dir)
			return -ENOMEM;
		mm_init_process_page_dir(child->memory->page_dir);
	}
	if (flags & CLONE_FS) {
		child->fs = ref_count_get(parent->fs);
	} else
		child->fs = alloc_fs(parent->fs);
	if (flags & CLONE_THREAD) {
		child->thread = ref_count_get(parent->thread);
	} else
		child->thread = alloc_thread_group(parent->thread);
	if (flags & CLONE_SIGHAND) {
		child->sighand = ref_count_get(parent->sighand);
	} else
		child->sighand = ps_copy_sighand(parent->sighand);
	if (!child->fs || !child->thread || !child->sighand)
		return -ENOMEM;
	if (!(flags & CLONE_THREAD))
		child->thread->tgid = child->life->psid;
	child->signal->sig_mask = parent->signal->sig_mask;
	child->signal->altstack = parent->signal->altstack;
	if ((flags & CLONE_VM) && !(flags & CLONE_VFORK))
		child->signal->altstack = (stack_t){ .ss_flags = SS_DISABLE };
	if (ps_dup_fds(parent, child, !!(flags & CLONE_FILES)) ||
	    fork_dup_io(parent, child))
		return -ENOMEM;
	if (flags & CLONE_VM) {
		ps_copy_thread_state(parent, child);
	} else {
		/* Keep VM metadata, LDT, and the COW mapping snapshot consistent.
		 * File/FS copies finish first to preserve the FS-to-VM lock order. */
		LOCK_GUARD(&parent->memory->mapping_lock);
		ps_copy_thread_state(parent, child);
		if (copy_page_range(parent, child))
			return -ENOMEM;
	}
	return 0;
}

/* Exec detaches objects shared with CLONE_VM/CLONE_FS/CLONE_SIGHAND peers. */
int ps_unshare_exec_context(task_struct *task)
{
	task_fs *fs = NULL;
	signal_handlers *handlers = NULL;
	if (ref_count_read(task->fs) > 1 && !(fs = alloc_fs(task->fs)))
		goto nomem;
	if (ref_count_read(task->sighand) > 1 &&
	    !(handlers = ps_copy_sighand(task->sighand)))
		goto nomem;
	if (fs) {
		ref_count_put(task->fs);
		task->fs = fs;
	}
	if (handlers) {
		ref_count_put(task->sighand);
		task->sighand = handlers;
	}
	return 0;
nomem:
	if (fs)
		ref_count_put(fs);
	if (handlers)
		ref_count_put(handlers);
	return -ENOMEM;
}
