#ifndef _LIB_LOCK_H
#define _LIB_LOCK_H
#include <lib/list.h>

/* ===========================================================================
 * Spinlock
 *
 * A raw test-and-set spinlock.  Optionally disables local interrupts while
 * held so that interrupt handlers on the same CPU cannot dead-lock against
 * kernel code that is spinning.
 *
 * NOTE: on SMP the int_status field is stored in the lock struct, so only
 * one CPU may use spinlock_lock/spinlock_unlock on the same lock instance
 * at a time (which is trivially true because only the holder calls unlock).
 * Contending CPUs spin with interrupts already disabled.
 * ===========================================================================*/

typedef volatile struct _spinlock {
	unsigned int lock; /* 0 = free, 1 = held (TAS word)         */
	int inited; /* 1 after spinlock_init                  */
	unsigned old_int; /* interrut status before locking */
	const char *holder;
} spinlock_t;

void spinlock_init(spinlock_t *lock);
void spinlock_uninit(spinlock_t *lock);
#define spinlock_lock(x) _spinlock_lock((x), __func__)
void _spinlock_lock(spinlock_t *lock, const char *func);
void spinlock_unlock(spinlock_t *lock);

/* ===========================================================================
 * lock_base  (internal)
 *
 * Binary semaphore (0 = available, 1 = taken) with a wait queue.
 * cond_t and mutex_t are built on top of this.
 * ===========================================================================*/

typedef struct _lock_base {
	unsigned int lock; /* 0 = available, 1 = taken               */
	list_entry wait_list; /* queue of sleeping task_struct entries   */
	spinlock_t wait_lock; /* guards wait_list and lock transitions   */
} lock_base;

/* ===========================================================================
 * Condition variable (cond_t)
 *
 * Binary event: callers wait until the event fires (lock==0); the notifier
 * clears the event and wakes one waiter.
 *
 * cond_wait_at_intr / cond_notify_at_intr are lightweight polling variants
 * for use in interrupt context where sleeping is not permitted.
 * ===========================================================================*/

typedef volatile struct _cond {
	lock_base base;
} cond_t;

void cond_init(cond_t *s, unsigned int initstat);
/* Block until the event fires.  If interruptible is non-zero, returns -1
 * immediately when a deliverable signal is pending; returns 0 on success. */
#define cond_wait(x, intr) _cond_wait((x), __func__, (intr))
int _cond_wait(cond_t *s, const char *func, int interruptible);
void cond_wait_at_intr(cond_t *s);
void cond_reset(cond_t *s);
void cond_notify(cond_t *s);
void cond_notify_at_intr(cond_t *s);

/* ===========================================================================
 * Mutex (mutex_t)
 *
 * Non-recursive mutual exclusion lock.  Only the holder may call
 * mutex_unlock; attempting to unlock from a different task triggers DIE().
 * ===========================================================================*/

typedef volatile struct _mutex {
	lock_base base;
	unsigned holder; /* psid of the holding task, 0 if free     */
	const char *holder_func; /* name of the holding task, 0 if free     */

} mutex_t;

void mutex_init(mutex_t *m);
#define mutex_lock(x) _mutex_lock((x), __func__)
void _mutex_lock(mutex_t *m, const char *func);
void mutex_unlock(mutex_t *m);

/* ===========================================================================
 * Readers-writer lock (rwlock_t)
 *
 * Write-preferring: once a writer is waiting, new readers queue behind it
 * to prevent writer starvation from a continuous stream of readers.
 *
 *   - Multiple readers may hold the lock concurrently.
 *   - A writer gets exclusive access (no concurrent readers or writers).
 * ===========================================================================*/

typedef volatile struct _rwlock {
	int readers; /* number of active readers             */
	int writer; /* 1 = a writer currently holds lock    */
	int writers_waiting; /* number of writers queued             */
	list_entry reader_wait_list; /* readers blocked on a writer          */
	list_entry writer_wait_list; /* writers blocked on readers/writer    */
	spinlock_t wait_lock; /* guards all fields above              */
} rwlock_t;

void rwlock_init(rwlock_t *rw);
#define rwlock_read_lock(x) _rwlock_read_lock((x), __func__)
void _rwlock_read_lock(rwlock_t *rw, const char *func);
void rwlock_read_unlock(rwlock_t *rw);
#define rwlock_write_lock(x) _rwlock_write_lock((x), __func__)
void _rwlock_write_lock(rwlock_t *rw, const char *func);
void rwlock_write_unlock(rwlock_t *rw);

/* ===========================================================================
 * Semaphore (sem_t)
 *
 * Counting semaphore.  sem_wait decrements the count and blocks when the
 * count is zero; sem_post increments the count and wakes one waiter.
 * ===========================================================================*/

typedef volatile struct _sem {
	int count; /* available resource count               */
	list_entry wait_list; /* tasks sleeping in sem_wait             */
	spinlock_t wait_lock; /* guards count and wait_list             */
} sem_t;

void sem_init(sem_t *s, int count);
#define sem_wait(x) _sem_wait((x), __func__)
void _sem_wait(sem_t *s, const char *func);
void sem_post(sem_t *s);

/* Interrupt-compatible pair: the waiter stays in the ready queue (polls via
 * task_sched) so the poster only needs an atomic increment.  Both sides must
 * use the _at_intr variants together; mixing with sem_wait/sem_post is unsafe. */
void sem_wait_at_intr(sem_t *s);
void sem_post_at_intr(sem_t *s);

#endif
