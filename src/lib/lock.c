#include <macro.h>
#include <lock.h>
#include <ps.h>
#include <klib.h>
#include <int.h>

static void waitable_get(void *waitable);

static void waitable_put(void *waitable);

void spinlock_init(spinlock_t *lock)
{
	lock->lock = 0;
	lock->inited = 1;
	lock->int_status = 0;
	lock->disable_intr = 1;
}

void spinlock_init_ex(spinlock_t *lock, int disable_intr)
{
	spinlock_init(lock);
	lock->disable_intr = disable_intr;
}

void spinlock_uninit(spinlock_t *lock)
{
	lock->inited = 0;
	__sync_lock_test_and_set(&(lock->lock), 0);
	if (lock->int_status == 1)
		int_intr_enable();
	else
		int_intr_disable();
}

void spinlock_lock(spinlock_t *lock)
{
	if (lock->inited) {
		if (lock->disable_intr)
			lock->int_status = int_intr_disable();

		while (__sync_lock_test_and_set(&(lock->lock), 1) == 1)
			HLT();
	}
}

void spinlock_unlock(spinlock_t *lock)
{
	if (lock->inited) {
		// spinlock_put(&lock->lock);
		__sync_lock_test_and_set(&(lock->lock), 0);

		if (lock->disable_intr) {
			if (lock->int_status == 1) {
				int_intr_enable();
			} else {
				int_intr_disable();
			}
		}
	}
}

static void lock_init(lock_base *b, unsigned int initstat)
{
	b->lock = initstat;
	list_init(&b->wait_list);
	spinlock_init(&b->wait_lock);
}

void lock_wait(lock_base *s)
{
	task_struct *cur = CURRENT_TASK();
	while (__sync_lock_test_and_set(&(s->lock), 1) == 1) {
		// change myself into waiting status
		spinlock_lock(&s->wait_lock);
		cur->status = ps_waiting;
		ps_put_to_wait_queue(cur);
		// then add to cond wait list
		list_insert_tail(&s->wait_list, &cur->lock_list);
		spinlock_unlock(&s->wait_lock);
		task_sched();
	}
}

static int lock_lease_one(lock_base *s)
{
	task_struct *task = 0;
	list_entry *entry = 0;
	int has = 0;
	spinlock_lock(&s->wait_lock);
	// get a waiting task is has
	if (!list_is_empty(&s->wait_list)) {
		entry = list_remove_head(&s->wait_list);
		task = container_of(entry, task_struct, lock_list);
		// then put it into ready status
		task->status = ps_ready;
		ps_put_to_ready_queue(task);
		has = 1;
	}
	spinlock_unlock(&s->wait_lock);
	return has;
}

void cond_init(cond_t *s, const char *name, unsigned int initstat)
{
	if (name && *name)
		strcpy(s->name, name);
	else
		*s->name = '\0';

	lock_init(&s->base, initstat);
}

void cond_wait(cond_t *s)
{
	lock_wait(&s->base);
}

void cond_reset(cond_t *s)
{
	__sync_lock_test_and_set(&(s->base.lock), 1);
}

void cond_notify(cond_t *s)
{
	int has = lock_lease_one(&s->base);

	__sync_lock_test_and_set(&(s->base.lock), 0);

	if (has)
		task_sched();
}

void cond_wait_at_intr(cond_t *s)
{
	while (__sync_lock_test_and_set(&(s->base.lock), 1) == 1) {
		task_sched();
	}
}

void cond_notify_at_intr(cond_t *s)
{
	__sync_lock_test_and_set(&(s->base.lock), 0);
}

void mutex_init(mutex_t *m)
{
	lock_init(&m->base, 0);
	m->holder = 0;
}

void mutex_lock(mutex_t *m)
{
	task_struct *cur = CURRENT_TASK();
	lock_wait(&m->base);
	m->holder = cur->psid;
}

void mutex_unlock(mutex_t *m)
{
	task_struct *cur = CURRENT_TASK();
	if (m->holder != cur->psid) {
		DIE();
	}

	lock_lease_one(&m->base);
	__sync_lock_test_and_set(&(m->base.lock), 0);
}