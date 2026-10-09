#include <int/int.h>
#include <device/time.h>
#include <ps/ps.h>
#include <ps/smp.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <macro.h>

const lock_operations_t spinlock_guard_operations;
const lock_operations_t mutex_guard_operations;
const lock_operations_t rmutex_guard_operations;
const lock_operations_t rwlock_guard_operations;
const lock_operations_t vm_guard_operations;

/* ===========================================================================
 * Spinlock
 * ===========================================================================*/

void spinlock_init(spinlock_t *lock)
{
	lock->header.operations = &spinlock_guard_operations;
	lock->lock = 0;
	lock->inited = 1;
	lock->holder = 0;
}

void spinlock_uninit(spinlock_t *lock)
{
	lock->inited = 0;
	lock->holder = 0;
	__atomic_store_n(&lock->lock, 0, __ATOMIC_RELEASE);
}

void _spinlock_lock(spinlock_t *lock, volatile int *saved_irq, const char *func)
{
	if (!lock->inited)
		return;

	/* Caller-local state: this store needs no atomic read/modify/write. */
	*saved_irq = int_intr_disable();

	/* Fast path: one acquire operation suffices without contention. */
	if (LIKELY(__atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE) == 0))
		goto locked;

	/* Read-only polling avoids bouncing the cache line while held. A
	 * bounded backoff also spreads out competing acquisitions. Keep
	 * servicing shootdowns on every pause while interrupts are disabled. */
	unsigned backoff = 1;
	for (;;) {
		for (unsigned i = 0; i < backoff; i++) {
			smp_tlb_poll();
			PAUSE();
		}
		if (__atomic_load_n(&lock->lock, __ATOMIC_RELAXED) == 0 &&
		    __atomic_exchange_n(&lock->lock, 1, __ATOMIC_ACQUIRE) == 0)
			break;
		if (backoff < 32)
			backoff <<= 1;
	}

locked:
	lock->holder = func;
}

void spinlock_unlock(spinlock_t *lock, int irq)
{
	if (!lock->inited)
		return;

	lock->holder = (const char *)0xff;
	__atomic_store_n(&lock->lock, 0, __ATOMIC_RELEASE);
	int_intr_setlevel(irq);
}

/* ===========================================================================
 * lock_base  (internal)
 * ===========================================================================*/

static void lock_init(lock_base *b, unsigned int initstat)
{
	b->lock = initstat;
	b->waiters = 0;
	list_init(&b->wait_list);
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

	entry = list_remove_head(wait_list);
	task = container_of(entry, task_struct, ps_list);
	ps_put_to_ready_queue(task);
	return 1;
}

/* Avoid an atomic write when an already-owned lock is being probed. */
static int lock_try_acquire(lock_base *s)
{
	unsigned int expected = 0;
	return __atomic_load_n(&s->lock, __ATOMIC_RELAXED) == 0 &&
	       __atomic_compare_exchange_n(&s->lock, &expected, 1, 0,
					   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* Registration precedes the inner state check. Paired sequentially
 * consistent operations in release prevent both sides from missing each
 * other; the queue lock then serializes enqueue with dequeue. Registrations
 * include woken tasks until they acquire or abandon their wait. */
static int lock_try_acquire_registered(lock_base *s)
{
	unsigned int expected = 0;
	return __atomic_compare_exchange_n(&s->lock, &expected, 1, 0,
					   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void lock_base_acquire(lock_base *s, const char *func)
{
	if (LIKELY(lock_try_acquire(s)))
		return;

	task_struct *cur = CURRENT_TASK();
	int irq;
	__atomic_add_fetch(&s->waiters, 1, __ATOMIC_SEQ_CST);
	for (;;) {
		spinlock_lock(&s->wait_lock, &irq);
		if (lock_try_acquire_registered(s)) {
			__atomic_sub_fetch(&s->waiters, 1, __ATOMIC_SEQ_CST);
			spinlock_unlock(&s->wait_lock, irq);
			return;
		}
		ps_put_to_wait_queue(cur, &s->wait_list, func);
		spinlock_unlock(&s->wait_lock, irq);
		task_sched();
	}
}

static void lock_base_release(lock_base *s)
{
	int irq;
	__atomic_store_n(&s->lock, 0, __ATOMIC_SEQ_CST);
	if (LIKELY(__atomic_load_n(&s->waiters, __ATOMIC_SEQ_CST) == 0))
		return;
	spinlock_lock(&s->wait_lock, &irq);
	lock_wake_one_locked(&s->wait_list);
	spinlock_unlock(&s->wait_lock, irq);
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
	int irq;

	if (LIKELY(lock_try_acquire(b)))
		return 0;

	__atomic_add_fetch(&b->waiters, 1, __ATOMIC_SEQ_CST);
	for (;;) {
		spinlock_lock(&b->wait_lock, &irq);
		if (lock_try_acquire_registered(b)) {
			__atomic_sub_fetch(&b->waiters, 1, __ATOMIC_SEQ_CST);
			spinlock_unlock(&b->wait_lock, irq);
			return 0;
		}
		if (interruptible) {
			if (ps_prepare_interruptible_wait(cur, &b->wait_list, 0,
							  func) < 0) {
				__atomic_sub_fetch(&b->waiters, 1,
						   __ATOMIC_SEQ_CST);
				spinlock_unlock(&b->wait_lock, irq);
				return -1;
			}
		} else {
			ps_put_to_wait_queue(cur, &b->wait_list, func);
		}
		spinlock_unlock(&b->wait_lock, irq);
		task_sched();

		if (interruptible)
			ps_finish_timed_wait(cur);
		if (interruptible && ps_interrupting_signals(cur)) {
			/* A signalled waiter may have consumed the only wakeup.
			 * Pass it on if the event is still available. */
			spinlock_lock(&b->wait_lock, &irq);
			__atomic_sub_fetch(&b->waiters, 1, __ATOMIC_SEQ_CST);
			if (__atomic_load_n(&b->lock, __ATOMIC_ACQUIRE) == 0)
				lock_wake_one_locked(&b->wait_list);
			spinlock_unlock(&b->wait_lock, irq);
			return -1;
		}
	}
}

/* Re-arm the event so the next cond_wait will block (lock = 1). */
void cond_reset(cond_t *s)
{
	__atomic_store_n(&s->base.lock, 1, __ATOMIC_RELAXED);
}

/* Fire the event: clear the lock and wake one sleeping waiter. */
void cond_notify(cond_t *s)
{
	lock_base_release((lock_base *)&s->base);
}

/* Interrupt-context variants: poll instead of sleep. */
void cond_wait_at_intr(cond_t *s)
{
	while (!lock_try_acquire((lock_base *)&s->base)) {
		smp_tlb_poll();
		PAUSE();
	}
}

void cond_notify_at_intr(cond_t *s)
{
	__atomic_store_n(&s->base.lock, 0, __ATOMIC_RELEASE);
}

/* ===========================================================================
 * Mutex (mutex_t)
 * ===========================================================================*/

void mutex_init(mutex_t *m)
{
	m->header.operations = &mutex_guard_operations;
	lock_init((lock_base *)&m->base, 0);
	__atomic_store_n(&m->holder, 0, __ATOMIC_RELAXED);
	m->holder_func = NULL;
}

void _mutex_lock(mutex_t *m, const char *func)
{
	task_struct *cur = CURRENT_TASK();
	lock_base_acquire((lock_base *)&m->base, func);
	__atomic_store_n(&m->holder, cur->psid, __ATOMIC_RELAXED);
	m->holder_func = func;
}

void mutex_unlock(mutex_t *m)
{
	task_struct *cur = CURRENT_TASK();

	if (m->holder != cur->psid)
		DIE();

	__atomic_store_n(&m->holder, 0, __ATOMIC_RELAXED);
	m->holder_func = NULL;

	lock_base_release((lock_base *)&m->base);
}

/* ===========================================================================
 * Recursive mutex (rmutex_t)
 * ===========================================================================*/

void rmutex_init(rmutex_t *m)
{
	m->header.operations = &rmutex_guard_operations;
	lock_init((lock_base *)&m->base, 0);
	__atomic_store_n(&m->holder, 0, __ATOMIC_RELAXED);
	m->depth = 0;
	m->holder_func = NULL;
}

void _rmutex_lock(rmutex_t *m, const char *func)
{
	task_struct *cur = CURRENT_TASK();

	/* Re-entrant: same task locks again, just deepen. */
	if (__atomic_load_n(&m->holder, __ATOMIC_RELAXED) == cur->psid) {
		m->depth++;
		return;
	}

	lock_base_acquire((lock_base *)&m->base, func);
	__atomic_store_n(&m->holder, cur->psid, __ATOMIC_RELAXED);
	m->depth = 1;
	m->holder_func = func;
}

void rmutex_unlock(rmutex_t *m)
{
	task_struct *cur = CURRENT_TASK();

	if (m->holder != cur->psid)
		DIE();

	if (--m->depth > 0)
		return;

	__atomic_store_n(&m->holder, 0, __ATOMIC_RELAXED);
	m->holder_func = NULL;

	lock_base_release((lock_base *)&m->base);
}

/* ===========================================================================
 * Readers-writer lock (rwlock_t)
 *
 * Write-preferring policy:
 *   rwlock_read_unlock  -> if readers == 0 and writers waiting: wake one writer
 *   rwlock_write_unlock -> if writers waiting: wake one writer
 *                          else:               wake all waiting readers
 * ===========================================================================*/

#define RW_WRITER (1U << 30)
#define RW_PENDING (1U << 31)
#define RW_READERS (RW_WRITER - 1)

void rwlock_init(rwlock_t *rw)
{
	rw->header.operations = &rwlock_guard_operations;
	rw->state = 0;
	rw->writers_waiting = 0;
	list_init((list_entry *)&rw->reader_wait_list);
	list_init((list_entry *)&rw->writer_wait_list);
	spinlock_init(&rw->wait_lock);
}

static int rwlock_try_read(rwlock_t *rw)
{
	unsigned int state = __atomic_load_n(&rw->state, __ATOMIC_RELAXED);
	while (!(state & (RW_WRITER | RW_PENDING))) {
		if ((state & RW_READERS) == RW_READERS)
			DIE();
		if (__atomic_compare_exchange_n(&rw->state, &state, state + 1,
						1, __ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED))
			return 1;
		PAUSE();
	}
	return 0;
}

void _rwlock_read_lock(rwlock_t *rw, const char *func)
{
	if (LIKELY(rwlock_try_read(rw)))
		return;

	task_struct *cur = CURRENT_TASK();
	int irq;
	spinlock_lock(&rw->wait_lock, &irq);
	while (!rwlock_try_read(rw)) {
		ps_put_to_wait_queue(cur, (list_entry *)&rw->reader_wait_list,
				     func);
		spinlock_unlock(&rw->wait_lock, irq);
		task_sched();
		spinlock_lock(&rw->wait_lock, &irq);
	}
	spinlock_unlock(&rw->wait_lock, irq);
}

void rwlock_read_unlock(rwlock_t *rw)
{
	unsigned int state =
		__atomic_fetch_sub(&rw->state, 1, __ATOMIC_RELEASE);
	/* Only the last reader with a pending writer needs the queue lock. */
	if (LIKELY(state != (RW_PENDING | 1)))
		return;

	int irq;
	spinlock_lock(&rw->wait_lock, &irq);
	if (__atomic_load_n(&rw->state, __ATOMIC_ACQUIRE) == RW_PENDING)
		lock_wake_one_locked((list_entry *)&rw->writer_wait_list);
	spinlock_unlock(&rw->wait_lock, irq);
}

void _rwlock_write_lock(rwlock_t *rw, const char *func)
{
	unsigned int state = 0;
	if (LIKELY(__atomic_compare_exchange_n(&rw->state, &state, RW_WRITER, 0,
					       __ATOMIC_ACQUIRE,
					       __ATOMIC_RELAXED)))
		return;

	task_struct *cur = CURRENT_TASK();
	int irq;
	spinlock_lock(&rw->wait_lock, &irq);
	rw->writers_waiting++;
	/* Close the reader gate in the same word used by reader CAS. Readers
	 * already admitted drain normally; later readers cannot bypass us. */
	__atomic_fetch_or(&rw->state, RW_PENDING, __ATOMIC_RELAXED);
	for (;;) {
		state = RW_PENDING;
		if (__atomic_compare_exchange_n(
			    &rw->state, &state, RW_PENDING | RW_WRITER, 0,
			    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			break;
		ps_put_to_wait_queue(cur, (list_entry *)&rw->writer_wait_list,
				     func);
		spinlock_unlock(&rw->wait_lock, irq);
		task_sched();
		spinlock_lock(&rw->wait_lock, &irq);
	}
	if (--rw->writers_waiting == 0)
		__atomic_fetch_and(&rw->state, ~RW_PENDING, __ATOMIC_RELAXED);
	spinlock_unlock(&rw->wait_lock, irq);
}

void rwlock_write_unlock(rwlock_t *rw)
{
	int irq;
	spinlock_lock(&rw->wait_lock, &irq);
	__atomic_fetch_and(&rw->state, ~RW_WRITER, __ATOMIC_RELEASE);
	if (rw->writers_waiting > 0) {
		lock_wake_one_locked((list_entry *)&rw->writer_wait_list);
	} else {
		while (lock_wake_one_locked(
			(list_entry *)&rw->reader_wait_list))
			;
	}
	spinlock_unlock(&rw->wait_lock, irq);
}

/* ===========================================================================
 * Semaphore (sem_t)
 * ===========================================================================*/

void sem_init(sem_t *s, int count)
{
	s->count = count;
	s->waiters = 0;
	list_init((list_entry *)&s->wait_list);
	spinlock_init((spinlock_t *)&s->wait_lock);
}

static inline int sem_try_wait(sem_t *s, int order)
	__attribute__((always_inline));
static inline int sem_try_wait(sem_t *s, int order)
{
	int count = __atomic_load_n(&s->count, order);
	while (count > 0) {
		if (__atomic_compare_exchange_n(&s->count, &count, count - 1, 1,
						order, order))
			return 1;
		PAUSE();
	}
	return 0;
}

void _sem_wait(sem_t *s, const char *func)
{
	if (LIKELY(sem_try_wait(s, __ATOMIC_ACQUIRE)))
		return;

	task_struct *cur = CURRENT_TASK();
	int irq;
	__atomic_add_fetch(&s->waiters, 1, __ATOMIC_SEQ_CST);
	for (;;) {
		spinlock_lock((spinlock_t *)&s->wait_lock, &irq);
		if (sem_try_wait(s, __ATOMIC_SEQ_CST)) {
			__atomic_sub_fetch(&s->waiters, 1, __ATOMIC_SEQ_CST);
			spinlock_unlock((spinlock_t *)&s->wait_lock, irq);
			return;
		}
		ps_put_to_wait_queue(cur, (list_entry *)&s->wait_list, func);
		spinlock_unlock((spinlock_t *)&s->wait_lock, irq);
		task_sched();
	}
}

void sem_post(sem_t *s)
{
	int irq;
	__atomic_fetch_add(&s->count, 1, __ATOMIC_SEQ_CST);
	if (LIKELY(__atomic_load_n(&s->waiters, __ATOMIC_SEQ_CST) == 0))
		return;
	spinlock_lock((spinlock_t *)&s->wait_lock, &irq);
	lock_wake_one_locked((list_entry *)&s->wait_list);
	spinlock_unlock((spinlock_t *)&s->wait_lock, irq);
}

/* Polling variants deliberately do not manipulate sleeping wait queues. */
void sem_wait_at_intr(sem_t *s)
{
	while (!sem_try_wait(s, __ATOMIC_ACQUIRE)) {
		smp_tlb_poll();
		PAUSE();
	}
}

void sem_post_at_intr(sem_t *s)
{
	__atomic_fetch_add(&s->count, 1, __ATOMIC_RELEASE);
}

/* VM transactions publish ownership before restoring local interrupts. */
void vm_lock_init(rmutex_t *lock)
{
	rmutex_init(lock);
	lock->header.operations = &vm_guard_operations;
}

void vm_lock_enter(rmutex_t *lock, const char *func)
{
	unsigned irq = int_intr_disable();
	_rmutex_lock(lock, func);
	__sync_fetch_and_add(&current->vm_lock_depth, 1);
	int_intr_setlevel(irq);
}

void vm_lock_leave(rmutex_t *lock)
{
	unsigned irq = int_intr_disable();
	rmutex_unlock(lock);
	__sync_fetch_and_sub(&current->vm_lock_depth, 1);
	int_intr_setlevel(irq);
}

static int guard_mutex_enter(void *lock, const char *func)
{
	_mutex_lock(lock, func);
	return 0;
}

static void guard_mutex_leave(void *lock, int state __attribute__((unused)))
{
	mutex_unlock(lock);
}

static int guard_rmutex_enter(void *lock, const char *func)
{
	_rmutex_lock(lock, func);
	return 0;
}

static void guard_rmutex_leave(void *lock, int state __attribute__((unused)))
{
	rmutex_unlock(lock);
}

static int guard_vm_enter(void *lock, const char *func)
{
	vm_lock_enter(lock, func);
	return 0;
}

static void guard_vm_leave(void *lock, int state __attribute__((unused)))
{
	vm_lock_leave(lock);
}

static int guard_spinlock_enter(void *lock, const char *func)
{
	int irq = 0;
	_spinlock_lock(lock, &irq, func);
	return irq;
}

static void guard_spinlock_leave(void *lock, int state)
{
	spinlock_unlock(lock, state);
}

static int guard_rwlock_enter(void *lock, const char *func)
{
	_rwlock_write_lock(lock, func);
	return 0;
}

static void guard_rwlock_leave(void *lock, int state __attribute__((unused)))
{
	rwlock_write_unlock(lock);
}

const lock_operations_t mutex_guard_operations = { guard_mutex_enter,
						   guard_mutex_leave };
const lock_operations_t rmutex_guard_operations = { guard_rmutex_enter,
						    guard_rmutex_leave };
const lock_operations_t vm_guard_operations = { guard_vm_enter,
						guard_vm_leave };
const lock_operations_t spinlock_guard_operations = { guard_spinlock_enter,
						      guard_spinlock_leave };
const lock_operations_t rwlock_guard_operations = { guard_rwlock_enter,
						    guard_rwlock_leave };
