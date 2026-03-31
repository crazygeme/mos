#include <hw/time.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <macro.h>

/* ===========================================================================
 * Spinlock
 * ===========================================================================*/

void spinlock_init(spinlock_t *lock)
{
	lock->lock = 0;
	lock->inited = 1;
}

void spinlock_uninit(spinlock_t *lock)
{
	lock->inited = 0;
	/* Release the lock word and restore the saved interrupt level. */
	__sync_lock_release(&lock->lock);
}

void _spinlock_lock(spinlock_t *lock, const char *func)
{
	if (!lock->inited)
		return;

	/* Fast path: optimistically try once before entering the retry loop.
	 * On an uncontended lock this avoids the PAUSE overhead entirely. */
	if (__builtin_expect(__sync_lock_test_and_set(&lock->lock, 1) == 0, 1))
		goto locked;

	/* Slow path: spin with PAUSE to yield the CPU pipeline and reduce
	 * memory bus contention on the lock cache line. */
	do {
		PAUSE();
	} while (__sync_lock_test_and_set(&lock->lock, 1) == 1);

locked:
	lock->old_int = sched_disable();
	lock->holder = func;
}

void spinlock_unlock(spinlock_t *lock)
{
	if (!lock->inited)
		return;

	lock->holder = 0xff;
	sched_set_level(lock->old_int);
	lock->old_int = 1;
	__sync_lock_test_and_set(&lock->lock, 0);
}

/* ===========================================================================
 * lock_base  (internal)
 * ===========================================================================*/

static void lock_init(lock_base *b, unsigned int initstat)
{
	b->lock = initstat;
	list_init(&b->wait_list);
	/* The wait_lock itself must not disable interrupts; cond_notify and
	 * mutex_unlock may be called from contexts where interrupts are
	 * already managed by an outer spinlock. */
	spinlock_init(&b->wait_lock);
}

/*
 * Wake one task from @list (caller must hold the enclosing wait_lock).
 * Returns 1 if a task was woken, 0 if the list was empty.
 */
static int lock_wake_one_locked(list_entry *wait_list)
{
	list_entry *entry;
	task_struct *task;

	if (list_is_empty(wait_list))
		return 0;

	entry = list_remove_tail(wait_list);
	task = container_of(entry, task_struct, ps_list);
	task->status = ps_ready;
	ps_put_to_ready_queue(task);
	return 1;
}

/*
 * Acquire binary lock @s, sleeping if it is already taken.
 *
 * Lost-wakeup fix: the lock state is re-checked while holding wait_lock
 * before the current task is enqueued.  The paired release in
 * lock_base_release_locked also runs under wait_lock, so one of these two
 * orderings always holds:
 *
 *   Acquirer sees lock==0 on the inner CAS  -> returns without sleeping
 *   Acquirer enqueues before releaser scans -> releaser wakes acquirer
 */
static void lock_base_acquire(lock_base *s, const char *func)
{
	task_struct *cur = CURRENT_TASK();

	while (1) {
		/* Fast path: uncontended. */
		if (__sync_lock_test_and_set(&s->lock, 1) == 0)
			return;

		/* Slow path: take wait_lock to avoid a lost wakeup. */
		spinlock_lock(&s->wait_lock);

		/* Re-check: lock may have been released while we took wait_lock. */
		if (__sync_lock_test_and_set(&s->lock, 1) == 0) {
			spinlock_unlock(&s->wait_lock);
			return;
		}

		ps_put_to_wait_queue(cur, (list_entry *)&s->wait_list, func);
		spinlock_unlock(&s->wait_lock);
		task_sched();
		/* After wakeup, retry from the top of the loop. */
	}
}

/*
 * Release binary lock @s and wake one waiter.
 * Caller must hold s->wait_lock; must unlock it after returning.
 *
 * The lock word is cleared first so that when the woken task retries the
 * CAS in lock_base_acquire it immediately succeeds.
 */
static void lock_base_release_locked(lock_base *s)
{
	__sync_lock_release(&s->lock);
	lock_wake_one_locked(&s->wait_list);
}

/* ===========================================================================
 * Condition variable (cond_t)
 * ===========================================================================*/

void cond_init(cond_t *s, unsigned int initstat)
{
	lock_init((lock_base *)&s->base, initstat);
}

/*
 * Block until the event fires (lock transitions to 0 via cond_notify).
 * If interruptible is non-zero, returns -1 when a deliverable signal wakes
 * the task instead of re-blocking; returns 0 on normal acquisition.
 */
int _cond_wait(cond_t *s, const char *func, int interruptible)
{
	task_struct *cur = CURRENT_TASK();
	lock_base *b = (lock_base *)&s->base;

	while (1) {
		if (__sync_lock_test_and_set(&b->lock, 1) == 0)
			return 0;

		spinlock_lock(&b->wait_lock);
		if (__sync_lock_test_and_set(&b->lock, 1) == 0) {
			spinlock_unlock(&b->wait_lock);
			return 0;
		}
		ps_put_to_wait_queue(cur, &b->wait_list, func);
		spinlock_unlock(&b->wait_lock);
		task_sched();

		if (interruptible &&
		    (cur->signal->sig_pending & ~cur->signal->sig_mask))
			return -1;
	}
}

/* Re-arm the event so the next cond_wait will block (lock = 1). */
void cond_reset(cond_t *s)
{
	__sync_lock_test_and_set(&s->base.lock, 1);
}

/* Fire the event: clear the lock and wake one sleeping waiter. */
void cond_notify(cond_t *s)
{
	spinlock_lock(&s->base.wait_lock);
	/* lock_base_release_locked clears the lock then wakes one waiter. */
	lock_base_release_locked((lock_base *)&s->base);
	spinlock_unlock(&s->base.wait_lock);

	/* Yield so the woken task can run without waiting for a timer tick. */
	task_sched();
}

/* Interrupt-context variants: poll instead of sleep. */
void cond_wait_at_intr(cond_t *s)
{
	while (__sync_lock_test_and_set(&s->base.lock, 1) == 1)
		;
}

void cond_notify_at_intr(cond_t *s)
{
	__sync_lock_release(&s->base.lock);
}

/* ===========================================================================
 * Mutex (mutex_t)
 * ===========================================================================*/

void mutex_init(mutex_t *m)
{
	lock_init((lock_base *)&m->base, 0);
	m->holder = 0;
}

void _mutex_lock(mutex_t *m, const char *func)
{
	task_struct *cur = CURRENT_TASK();
	lock_base_acquire((lock_base *)&m->base, func);
	m->holder = cur->psid;
	m->holder_func = func;
}

void mutex_unlock(mutex_t *m)
{
	task_struct *cur = CURRENT_TASK();

	if (m->holder != cur->psid)
		DIE();

	m->holder = 0;
	m->holder_func = NULL;

	/* Release the lock and wake the next waiter atomically under
	 * wait_lock so that no wakeup is lost between the two steps. */
	spinlock_lock(&m->base.wait_lock);
	lock_base_release_locked((lock_base *)&m->base);
	spinlock_unlock(&m->base.wait_lock);
}

/* ===========================================================================
 * Readers-writer lock (rwlock_t)
 *
 * Write-preferring policy:
 *   rwlock_read_unlock  -> if readers == 0 and writers waiting: wake one writer
 *   rwlock_write_unlock -> if writers waiting: wake one writer
 *                          else:               wake all waiting readers
 * ===========================================================================*/

void rwlock_init(rwlock_t *rw)
{
	rw->readers = 0;
	rw->writer = 0;
	rw->writers_waiting = 0;
	list_init((list_entry *)&rw->reader_wait_list);
	list_init((list_entry *)&rw->writer_wait_list);
	spinlock_init(&rw->wait_lock);
}

void _rwlock_read_lock(rwlock_t *rw, const char *func)
{
	task_struct *cur = CURRENT_TASK();

	spinlock_lock(&rw->wait_lock);

	/* Block if a writer holds the lock or a writer is waiting (write-
	 * preferring: we queue behind the writer to prevent its starvation). */
	while (rw->writer || rw->writers_waiting > 0) {
		ps_put_to_wait_queue(cur, (list_entry *)&rw->reader_wait_list,
				     func);
		spinlock_unlock(&rw->wait_lock);
		task_sched();
		spinlock_lock(&rw->wait_lock);
	}

	rw->readers++;
	spinlock_unlock(&rw->wait_lock);
}

void rwlock_read_unlock(rwlock_t *rw)
{
	spinlock_lock(&rw->wait_lock);

	rw->readers--;

	/* Last reader leaving: hand off to a waiting writer if one exists. */
	if (rw->readers == 0 && rw->writers_waiting > 0)
		lock_wake_one_locked((list_entry *)&rw->writer_wait_list);

	spinlock_unlock(&rw->wait_lock);
}

void _rwlock_write_lock(rwlock_t *rw, const char *func)
{
	task_struct *cur = CURRENT_TASK();

	spinlock_lock(&rw->wait_lock);
	rw->writers_waiting++;

	/* Wait until the lock is completely idle (no readers, no other writer). */
	while (rw->writer || rw->readers > 0) {
		ps_put_to_wait_queue(cur, (list_entry *)&rw->writer_wait_list,
				     func);
		spinlock_unlock(&rw->wait_lock);
		task_sched();
		spinlock_lock(&rw->wait_lock);
	}

	rw->writers_waiting--;
	rw->writer = 1;
	spinlock_unlock(&rw->wait_lock);
}

void rwlock_write_unlock(rwlock_t *rw)
{
	spinlock_lock(&rw->wait_lock);

	rw->writer = 0;

	if (rw->writers_waiting > 0) {
		/* Prefer waking a waiting writer to avoid writer starvation. */
		lock_wake_one_locked((list_entry *)&rw->writer_wait_list);
	} else {
		/* No writers pending: release all queued readers simultaneously
		 * so they can proceed concurrently. */
		while (lock_wake_one_locked(
			(list_entry *)&rw->reader_wait_list))
			;
	}

	spinlock_unlock(&rw->wait_lock);
	/* Yield so newly woken tasks get CPU time without waiting for a tick. */
	task_sched();
}

/* ===========================================================================
 * Semaphore (sem_t)
 * ===========================================================================*/

void sem_init(sem_t *s, int count)
{
	s->count = count;
	list_init((list_entry *)&s->wait_list);
	spinlock_init((spinlock_t *)&s->wait_lock);
}

/*
 * Decrement the semaphore count.  If the count is already zero, sleep
 * until sem_post increments it.
 *
 * Lost-wakeup fix: count is re-checked under wait_lock before sleeping,
 * mirroring the same pattern used in lock_base_acquire.
 */
void _sem_wait(sem_t *s, const char *func)
{
	task_struct *cur = CURRENT_TASK();

	while (1) {
		spinlock_lock((spinlock_t *)&s->wait_lock);

		if (s->count > 0) {
			s->count--;
			spinlock_unlock((spinlock_t *)&s->wait_lock);
			return;
		}

		ps_put_to_wait_queue(cur, (list_entry *)&s->wait_list, func);
		spinlock_unlock((spinlock_t *)&s->wait_lock);
		task_sched();
	}
}

/*
 * Increment the semaphore count.  Wake one sleeper if any are queued.
 */
void sem_post(sem_t *s)
{
	spinlock_lock((spinlock_t *)&s->wait_lock);
	s->count++;
	lock_wake_one_locked((list_entry *)&s->wait_list);
	spinlock_unlock((spinlock_t *)&s->wait_lock);
}

/*
 * Interrupt-compatible pair.
 *
 * sem_wait_at_intr: poll by yielding via task_sched() without entering the
 * wait queue.  The task stays in the ready queue so sem_post_at_intr's
 * atomic increment is sufficient to unblock it on the next reschedule.
 *
 * sem_post_at_intr: only atomically increment the count.  Safe in interrupt
 * context because it never touches the wait queue or the spinlock.
 *
 * These two must always be used together; mixing with sem_wait/sem_post is
 * unsafe.
 */
void sem_wait_at_intr(sem_t *s)
{
	while (__sync_fetch_and_add(&s->count, 0) == 0)
		;
	__sync_fetch_and_sub(&s->count, 1);
}

void sem_post_at_intr(sem_t *s)
{
	__sync_fetch_and_add(&s->count, 1);
}