#include <ps/ps.h>
#include <ps/clone.h>
#include <ps/usage.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <lib/klib.h>
#include <test/test.h>
#include <src/ps/impl/ps_internal.h>

/* Prepare real child contexts without publishing a runnable stack. */
static task_struct *context_child(task_struct *parent, int share_vm,
				  int share_fs, int thread, int share_handlers)
{
	task_struct *child = fork_alloc_child(parent);
	if (!child)
		return NULL;
	unsigned long flags = (share_vm ? CLONE_VM : 0) |
			      (share_fs ? CLONE_FS : 0) |
			      (thread ? CLONE_THREAD : 0) |
			      (share_handlers ? CLONE_SIGHAND : 0);
	if (ps_clone_resources(parent, child, flags))
		goto fail;
	child->thread->tgid = thread ? parent->thread->tgid : child->life->psid;
	child->life->type = ps_user;
	child->sched->status = ps_stopped;
	child->life->fork_flag = thread ? FORK_FLAG_THREAD : 0;
	return child;
fail:
	fork_abort_child(child);
	return NULL;
}

KTEST(ProcessContext, SharingAndForkIsolation)
{
	task_struct *parent = current;
	task_struct *thread = context_child(parent, 1, 1, 1, 1);
	task_struct *child = context_child(parent, 0, 0, 0, 0);
	EXPECT_TRUE(thread && child);
	if (!thread || !child) {
		if (thread)
			fork_abort_child(thread);
		if (child)
			fork_abort_child(child);
		return -1;
	}
	EXPECT_TRUE(thread->fs == parent->fs);
	EXPECT_TRUE(thread->memory == parent->memory);
	EXPECT_TRUE(thread->thread == parent->thread);
	EXPECT_TRUE(thread->sighand == parent->sighand);
	EXPECT_TRUE(child->fs != parent->fs);
	EXPECT_TRUE(child->memory != parent->memory);
	EXPECT_TRUE(child->thread != parent->thread);
	EXPECT_TRUE(child->sighand != parent->sighand);
	EXPECT_EQ(child->thread->user_tickets, 0UL);
	EXPECT_EQ(child->thread->child_utime, 0UL);
	EXPECT_EQ(child->thread->alarm_expire_ms, 0ULL);
	EXPECT_TRUE(RB_EMPTY_NODE(&child->thread->alarm_rb));
	unsigned mask = parent->fs->umask;
	child->fs->umask = mask ^ 0777;
	EXPECT_EQ(parent->fs->umask, mask);
	unsigned long blocked = thread->signal->sig_mask;
	thread->signal->sig_mask ^= 1UL << (SIGUSR1 - 1);
	EXPECT_EQ(parent->signal->sig_mask, blocked);
	EXPECT_EQ(ps_unshare_exec_context(thread), 0);
	EXPECT_TRUE(thread->fs != parent->fs);
	EXPECT_TRUE(thread->memory == parent->memory);
	EXPECT_TRUE(thread->sighand != parent->sighand);
	EXPECT_TRUE(thread->thread == parent->thread);
	fork_abort_child(thread);
	fork_abort_child(child);
	return 0;
}

KTEST(ProcessContext, ChildTransientState)
{
	task_struct *parent = context_child(current, 0, 0, 0, 0);
	if (!parent)
		return -1;
	parent->execution->clear_child_tid = (int *)0x1234;
	parent->sched->net_core_depth = 3;
	parent->sched->vm_lock_depth = 2;
	parent->life->stop_signal = SIGSTOP;
	parent->life->stop_report_pending = 1;
	parent->signal->restore_sigmask = 1;
	parent->signal->saved_sigmask = 17;
	parent->signal->sig_pending = 1UL << (SIGUSR1 - 1);
	parent->signal->altstack = (stack_t){ (void *)0x2000, 0, 4096 };
	task_struct *thread = context_child(parent, 1, 1, 1, 1);
	task_struct *forked = context_child(parent, 0, 0, 0, 0);
	if (!thread || !forked) {
		if (thread)
			fork_abort_child(thread);
		if (forked)
			fork_abort_child(forked);
		fork_abort_child(parent);
		return -1;
	}
	EXPECT_TRUE(thread->execution->clear_child_tid == NULL);
	EXPECT_EQ(thread->sched->net_core_depth, 0U);
	EXPECT_EQ(thread->sched->vm_lock_depth, 0U);
	EXPECT_EQ(thread->life->stop_signal, 0U);
	EXPECT_EQ(thread->life->stop_report_pending, 0U);
	EXPECT_EQ(thread->signal->sig_pending, 0UL);
	EXPECT_EQ(thread->signal->restore_sigmask, 0);
	EXPECT_EQ(thread->signal->saved_sigmask, 0UL);
	EXPECT_EQ(thread->signal->altstack.ss_flags, SS_DISABLE);
	EXPECT_TRUE(thread->signal->altstack.ss_sp == NULL);
	EXPECT_TRUE(forked->signal->altstack.ss_sp == (void *)0x2000);
	fork_abort_child(thread);
	fork_abort_child(forked);
	fork_abort_child(parent);
	return 0;
}

KTEST(ProcessContext, PendingSignalsAndTimerPayloads)
{
	task_struct *leader = context_child(current, 0, 0, 0, 0);
	if (!leader)
		return -1;
	task_struct *peer = context_child(leader, 1, 1, 1, 1);
	if (!peer) {
		fork_abort_child(leader);
		return -1;
	}
	leader->signal->sig_mask = 1UL << (SIGUSR1 - 1);
	peer->signal->sig_mask = 0;
	int irq;
	spinlock_lock(&ps_lock, &irq);
	ps_add_mgr_unsafe(leader);
	ps_add_mgr_unsafe(peer);
	ps_queue_group_signal_unsafe(leader, SIGUSR1);
	spinlock_unlock(&ps_lock, irq);
	EXPECT_EQ(ps_interrupting_signals(leader), 0UL);
	EXPECT_EQ(ps_interrupting_signals(peer), 1UL << (SIGUSR1 - 1));
	EXPECT_EQ(ps_take_signal(peer, (sigset_t)-1, NULL, NULL), SIGUSR1);
	EXPECT_EQ(ps_pending_signals(leader), 0UL);
	uintptr_t wide = sizeof(uintptr_t) == 8 ?
				 (uintptr_t)0x1234567887654321ULL :
				 0x87654321U;
	ps_timer_notify(leader->life->psid, SIGRTMIN_KERNEL, 11, wide, 0);
	ps_timer_notify(leader->life->psid, SIGRTMIN_KERNEL, 12, 99, 0);
	int id;
	uintptr_t value;
	EXPECT_EQ(ps_take_signal(peer, (sigset_t)-1, &id, &value),
		  SIGRTMIN_KERNEL);
	EXPECT_EQ(id, 11);
	EXPECT_EQ(value, wide);
	EXPECT_NE(ps_pending_signals(leader), 0UL);
	EXPECT_EQ(ps_take_signal(peer, (sigset_t)-1, &id, &value),
		  SIGRTMIN_KERNEL);
	EXPECT_EQ(id, 12);
	EXPECT_EQ(value, (uintptr_t)99);
	EXPECT_EQ(ps_pending_signals(leader), 0UL);
	ps_timer_notify(leader->life->psid, SIGRTMIN_KERNEL, 13, 3, 1);
	EXPECT_EQ(ps_pending_signals(peer), 0UL);
	EXPECT_EQ(ps_take_signal(leader, (sigset_t)-1, &id, &value),
		  SIGRTMIN_KERNEL);
	EXPECT_EQ(id, 13);
	spinlock_lock(&ps_lock, &irq);
	ps_remove_mgr_unsafe(peer);
	ps_remove_mgr_unsafe(leader);
	spinlock_unlock(&ps_lock, irq);
	peer->life->psid = leader->life->psid = (unsigned)-1;
	fork_abort_child(peer);
	fork_abort_child(leader);
	return 0;
}

KTEST(ProcessContext, IndependentCloneFlags)
{
	const unsigned long modes[] = { 0,
					CLONE_VM,
					CLONE_FILES,
					CLONE_FS,
					CLONE_VM | CLONE_SIGHAND,
					CLONE_VM | CLONE_SIGHAND |
						CLONE_THREAD };
	task_struct *parent = current;
	for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		unsigned long flags = modes[i];
		task_struct *child = fork_alloc_child(parent);
		if (!child)
			return -1;
		if (ps_clone_resources(parent, child, flags)) {
			fork_abort_child(child);
			return -1;
		}
		EXPECT_EQ(child->memory == parent->memory,
			  !!(flags & CLONE_VM));
		EXPECT_EQ(child->files == parent->files,
			  !!(flags & CLONE_FILES));
		EXPECT_EQ(child->fs == parent->fs, !!(flags & CLONE_FS));
		EXPECT_EQ(child->sighand == parent->sighand,
			  !!(flags & CLONE_SIGHAND));
		EXPECT_EQ(child->thread == parent->thread,
			  !!(flags & CLONE_THREAD));
		EXPECT_TRUE(child->execution != parent->execution);
		EXPECT_TRUE(child->credentials != parent->credentials);
		EXPECT_TRUE(child->signal != parent->signal);
		EXPECT_TRUE(child->execution->mmap_cache == NULL);
		EXPECT_TRUE(child->execution->mmap_cache_vm == NULL);
		struct sigaction old, action = { .sa_handler = SIG_IGN },
				      observed;
		ps_signal_action(parent, SIGUSR1, NULL, &old);
		ps_signal_action(child, SIGUSR1, &action, NULL);
		ps_signal_action(parent, SIGUSR1, NULL, &observed);
		EXPECT_TRUE(
			observed.sa_handler ==
			((flags & CLONE_SIGHAND) ? SIG_IGN : old.sa_handler));
		ps_signal_action(parent, SIGUSR1, &old, NULL);
		fork_abort_child(child);
	}
	return 0;
}
