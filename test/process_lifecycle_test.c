#include <ps/ps.h>
#include <ps/clone.h>
#include <test/test.h>
#include <lib/klib.h>
#include <errno.h>
#include <src/ps/impl/ps_internal.h>

/* Stopped contexts exercise lifecycle transitions without running user stacks. */
static task_struct *lifecycle_task(task_struct *parent, int thread)
{
	task_struct *task = fork_alloc_child(parent);
	if (!task)
		return NULL;
	unsigned long flags = thread ? CLONE_VM | CLONE_FS | CLONE_FILES |
					       CLONE_SIGHAND | CLONE_THREAD :
				       0;
	if (ps_clone_resources(parent, task, flags) ||
	    fork_set_meta(parent, task, thread ? FORK_FLAG_THREAD : 0)) {
		fork_abort_child(task);
		return NULL;
	}
	task->sched->status = ps_stopped;
	if (thread) {
		task->life->creator = NULL;
		task->life->ppid = parent->life->ppid;
	}
	return task;
}

static void lifecycle_publish(task_struct *task)
{
	int irq;
	spinlock_lock(&ps_lock, &irq);
	ps_add_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
}

static void lifecycle_destroy(task_struct *task)
{
	if (!task)
		return;
	int irq;
	spinlock_lock(&ps_lock, &irq);
	list_remove_entry(&task->sched->ps_list);
	list_init(&task->sched->ps_list);
	ps_remove_mgr_unsafe(task);
	spinlock_unlock(&ps_lock, irq);
	task->life->psid = (unsigned)-1;
	fork_abort_child(task);
}

static unsigned list_count(list_entry *head)
{
	unsigned count = 0;
	for (list_entry *entry = head->next; entry != head; entry = entry->next)
		count++;
	return count;
}

KTEST(ProcessLifecycle, GroupMembershipAndChildOwnership)
{
	task_struct *parent = lifecycle_task(current, 0);
	ASSERT_NONNULL(parent);
	task_struct *peer = lifecycle_task(parent, 1);
	if (!peer) {
		lifecycle_destroy(parent);
		return -1;
	}
	task_struct *child = lifecycle_task(peer, 0);
	if (!child) {
		lifecycle_destroy(peer);
		lifecycle_destroy(parent);
		return -1;
	}
	lifecycle_publish(parent);
	lifecycle_publish(peer);
	lifecycle_publish(child);
	EXPECT_EQ(parent->thread->live_tasks, 2U);
	EXPECT_EQ(list_count(&parent->thread->members), 2U);
	EXPECT_EQ(list_count(&parent->thread->children), 1U);
	EXPECT_EQ(list_count(&peer->life->children), 1U);
	EXPECT_EQ(list_count(&parent->life->children), 0U);
	EXPECT_EQ(child->life->ppid, parent->life->psid);
	EXPECT_TRUE(child->life->parent_group == parent->thread);
	EXPECT_TRUE(peer->life->parent_group == NULL);
	lifecycle_destroy(child);
	EXPECT_EQ(list_count(&parent->thread->children), 0U);
	EXPECT_EQ(peer->life->nchildren, 0U);
	lifecycle_destroy(peer);
	EXPECT_EQ(parent->thread->live_tasks, 1U);
	lifecycle_destroy(parent);
	return 0;
}

KTEST(ProcessLifecycle, CreatorExitPreservesZombieParent)
{
	task_struct *parent = lifecycle_task(current, 0);
	ASSERT_NONNULL(parent);
	task_struct *peer = lifecycle_task(parent, 1);
	if (!peer) {
		lifecycle_destroy(parent);
		return -1;
	}
	task_struct *child = lifecycle_task(peer, 0);
	if (!child) {
		lifecycle_destroy(peer);
		lifecycle_destroy(parent);
		return -1;
	}
	lifecycle_publish(parent);
	lifecycle_publish(peer);
	lifecycle_publish(child);
	int irq;
	spinlock_lock(&ps_lock, &irq);
	ps_put_to_dying_queue_unsafe(child);
	spinlock_unlock(&ps_lock, irq);
	ps_reparent_children(peer);
	EXPECT_EQ(child->life->ppid, parent->life->psid);
	EXPECT_TRUE(child->life->parent_group == parent->thread);
	EXPECT_TRUE(child->life->creator == NULL);
	EXPECT_EQ(list_count(&parent->life->dying_queue), 1U);
	EXPECT_EQ(list_count(&parent->thread->children), 1U);
	EXPECT_EQ(list_count(&peer->life->children), 0U);
	EXPECT_EQ(child->thread->live_tasks, 0U);
	lifecycle_destroy(child);
	lifecycle_destroy(peer);
	lifecycle_destroy(parent);
	return 0;
}

KTEST(ProcessLifecycle, CreatorDeathSignalPreservesPPid)
{
	task_struct *parent = lifecycle_task(current, 0);
	ASSERT_NONNULL(parent);
	task_struct *peer = lifecycle_task(parent, 1);
	if (!peer) {
		lifecycle_destroy(parent);
		return -1;
	}
	task_struct *child = lifecycle_task(peer, 0);
	if (!child) {
		lifecycle_destroy(peer);
		lifecycle_destroy(parent);
		return -1;
	}
	lifecycle_publish(parent);
	lifecycle_publish(peer);
	lifecycle_publish(child);
	child->life->pdeath_signal = SIGUSR1;
	ps_reparent_children(peer);
	EXPECT_EQ(child->life->ppid, parent->thread->tgid);
	EXPECT_EQ(ps_pending_signals(child), 1UL << (SIGUSR1 - 1));
	EXPECT_TRUE(child->life->creator == NULL);
	lifecycle_destroy(child);
	lifecycle_destroy(peer);
	lifecycle_destroy(parent);
	return 0;
}

KTEST(ProcessLifecycle, FinalGroupTaskAdoptsLiveChild)
{
	task_struct *parent = lifecycle_task(current, 0);
	ASSERT_NONNULL(parent);
	task_struct *peer = lifecycle_task(parent, 1);
	if (!peer) {
		lifecycle_destroy(parent);
		return -1;
	}
	task_struct *child = lifecycle_task(peer, 0);
	if (!child) {
		lifecycle_destroy(peer);
		lifecycle_destroy(parent);
		return -1;
	}
	lifecycle_publish(parent);
	lifecycle_publish(peer);
	lifecycle_publish(child);
	ps_reparent_children(peer);
	EXPECT_EQ(child->life->ppid, parent->life->psid);
	int irq;
	spinlock_lock(&ps_lock, &irq);
	ps_mark_dying_unsafe(peer);
	EXPECT_EQ(child->life->ppid, parent->life->psid);
	ps_mark_dying_unsafe(parent);
	spinlock_unlock(&ps_lock, irq);
	ps_reparent_children(parent);
	EXPECT_EQ(child->life->ppid, 1U);
	EXPECT_EQ(list_count(&parent->thread->children), 0U);
	EXPECT_TRUE(child->life->parent_group == ps_find_process(1)->thread);
	lifecycle_destroy(child);
	lifecycle_destroy(peer);
	lifecycle_destroy(parent);
	return 0;
}

KTEST(ProcessLifecycle, ConcurrentExitCommitAdoptsChildren)
{
	task_struct *parent = lifecycle_task(current, 0);
	ASSERT_NONNULL(parent);
	task_struct *peer = lifecycle_task(parent, 1);
	if (!peer) {
		lifecycle_destroy(parent);
		return -1;
	}
	task_struct *child = lifecycle_task(peer, 0);
	if (!child) {
		lifecycle_destroy(peer);
		lifecycle_destroy(parent);
		return -1;
	}
	lifecycle_publish(parent);
	lifecycle_publish(peer);
	lifecycle_publish(child);
	/* Both tasks complete exit preparation before either commits its exit. */
	ps_reparent_children(parent);
	ps_reparent_children(peer);
	int irq;
	spinlock_lock(&ps_lock, &irq);
	ps_mark_dying_unsafe(parent);
	EXPECT_EQ(child->life->ppid, parent->life->psid);
	ps_mark_dying_unsafe(peer);
	EXPECT_EQ(parent->thread->live_tasks, 0U);
	EXPECT_EQ(child->life->ppid, 1U);
	EXPECT_TRUE(list_is_empty(&parent->thread->children));
	spinlock_unlock(&ps_lock, irq);
	lifecycle_destroy(child);
	lifecycle_destroy(peer);
	lifecycle_destroy(parent);
	return 0;
}

static int lifecycle_nohang(int pgrp)
{
	task_struct *zombie = lifecycle_task(current, 0);
	ASSERT_NONNULL(zombie);
	task_struct *live = lifecycle_task(current, 0);
	if (!live) {
		lifecycle_destroy(zombie);
		return -1;
	}
	zombie->thread->group_id = zombie->life->psid;
	live->thread->group_id = live->life->psid;
	lifecycle_publish(zombie);
	lifecycle_publish(live);
	zombie->life->exit_status = 42 << 8;
	int irq;
	spinlock_lock(&ps_lock, &irq);
	ps_put_to_dying_queue_unsafe(zombie);
	spinlock_unlock(&ps_lock, irq);
	unsigned zombie_pid = zombie->life->psid, live_pid = live->life->psid;
	int status = 0x1234;
	EXPECT_EQ(pgrp ? do_waitpid_pgrp(live_pid, &status, WNOHANG, NULL) :
			 do_waitpid(live_pid, &status, WNOHANG, NULL),
		  0);
	EXPECT_EQ(status, 0x1234);
	EXPECT_TRUE(ps_find_process(zombie_pid) == zombie);
	EXPECT_EQ(zombie->life->exit_ready, 1U);
	EXPECT_EQ(pgrp ? do_waitpid_pgrp(zombie_pid, &status, 0, NULL) :
			 do_waitpid(zombie_pid, &status, 0, NULL),
		  (int)zombie_pid);
	EXPECT_EQ(status, 42 << 8);
	EXPECT_TRUE(ps_find_process(zombie_pid) == NULL);
	lifecycle_destroy(live);
	return 0;
}

KTEST(ProcessLifecycle, PidNoHangPreservesUnmatchedZombie)
{
	return lifecycle_nohang(0);
}

KTEST(ProcessLifecycle, GroupNoHangPreservesUnmatchedZombie)
{
	return lifecycle_nohang(1);
}

KTEST(ProcessLifecycle, ThreadsAreNotWaitableChildren)
{
	task_struct *peer = lifecycle_task(current, 1);
	ASSERT_NONNULL(peer);
	lifecycle_publish(peer);
	int status = 0x1234;
	EXPECT_EQ(do_waitpid(peer->life->psid, &status, WNOHANG, NULL),
		  -ECHILD);
	EXPECT_EQ(status, 0x1234);
	lifecycle_destroy(peer);
	return 0;
}

KTEST(ProcessLifecycle, NonleaderGroupExitRetainsLeaderPid)
{
	task_struct *parent = lifecycle_task(current, 0);
	ASSERT_NONNULL(parent);
	task_struct *peer = lifecycle_task(parent, 1);
	if (!peer) {
		lifecycle_destroy(parent);
		return -1;
	}
	lifecycle_publish(parent);
	lifecycle_publish(peer);
	unsigned pid = parent->life->psid;
	ps_kill_thread_group(peer, 23 << 8);
	EXPECT_TRUE(ps_find_process(pid) == parent);
	EXPECT_EQ(parent->life->exit_ready, 1U);
	EXPECT_EQ(parent->thread->live_tasks, 1U);
	int status = 0x1234;
	EXPECT_EQ(do_waitpid(pid, &status, WNOHANG, NULL), 0);
	EXPECT_EQ(status, 0x1234);
	ps_reparent_children(peer);
	lifecycle_destroy(peer);
	EXPECT_EQ(do_waitpid(pid, &status, 0, NULL), (int)pid);
	EXPECT_EQ(status, 23 << 8);
	EXPECT_TRUE(ps_find_process(pid) == NULL);
	return 0;
}
