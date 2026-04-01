/*
 * test/lock_test.c — unit tests for src/lib/lock.c
 *
 * Covers: spinlock_init/lock/unlock, mutex_init/lock/unlock,
 *         rwlock_init/read_lock/read_unlock/write_lock/write_unlock,
 *         cond_init/reset/notify (uncontended, single-task paths only).
 *
 * All tests exercise the uncontended fast path on a single CPU so that
 * no blocking or cross-task scheduling is needed.
 */

#include <lib/lock.h>
#include <ps/ps.h>
#include <test/test.h>

/* ── spinlock ────────────────────────────────────────────────────── */

KTEST(lock, spinlock_init_state)
{
	spinlock_t s;
	spinlock_init(&s);
	EXPECT_EQ((int)s.inited, 1);
	EXPECT_EQ((int)s.lock, 0);
	return 0;
}

KTEST(lock, spinlock_lock_sets_word)
{
	spinlock_t s;
	int irq;

	spinlock_init(&s);
	spinlock_lock(&s, &irq);
	EXPECT_EQ((int)s.lock, 1);
	spinlock_unlock(&s, irq);
	return 0;
}

KTEST(lock, spinlock_unlock_clears_word)
{
	spinlock_t s;
	int irq;

	spinlock_init(&s);
	spinlock_lock(&s, &irq);
	spinlock_unlock(&s, irq);
	EXPECT_EQ((int)s.lock, 0);
	return 0;
}

KTEST(lock, spinlock_holder_set_on_lock)
{
	spinlock_t s;
	int irq;

	spinlock_init(&s);
	spinlock_lock(&s, &irq);
	EXPECT_NONNULL(s.holder);
	spinlock_unlock(&s, irq);
	return 0;
}

KTEST(lock, spinlock_holder_cleared_on_unlock)
{
	spinlock_t s;
	int irq;

	spinlock_init(&s);
	spinlock_lock(&s, &irq);
	spinlock_unlock(&s, irq);
	EXPECT_NULL(s.holder);
	return 0;
}

KTEST(lock, spinlock_reacquire)
{
	/* Lock can be taken again after unlock. */
	spinlock_t s;
	int irq;

	spinlock_init(&s);
	spinlock_lock(&s, &irq);
	spinlock_unlock(&s, irq);
	spinlock_lock(&s, &irq);
	EXPECT_EQ((int)s.lock, 1);
	spinlock_unlock(&s, irq);
	EXPECT_EQ((int)s.lock, 0);
	return 0;
}

/* ── mutex ───────────────────────────────────────────────────────── */

KTEST(lock, mutex_init_state)
{
	mutex_t m;
	mutex_init(&m);
	EXPECT_EQ((int)m.base.lock, 0);
	EXPECT_EQ((int)m.holder, 0);
	return 0;
}

KTEST(lock, mutex_lock_sets_holder)
{
	mutex_t m;
	mutex_init(&m);
	mutex_lock(&m);
	/* holder must equal the current task's psid */
	EXPECT_EQ((int)m.holder, (int)CURRENT_TASK()->psid);
	mutex_unlock(&m);
	return 0;
}

KTEST(lock, mutex_unlock_clears_holder)
{
	mutex_t m;
	mutex_init(&m);
	mutex_lock(&m);
	mutex_unlock(&m);
	EXPECT_EQ((int)m.holder, 0);
	return 0;
}

KTEST(lock, mutex_lock_sets_base_lock)
{
	mutex_t m;
	mutex_init(&m);
	mutex_lock(&m);
	EXPECT_EQ((int)m.base.lock, 1);
	mutex_unlock(&m);
	return 0;
}

KTEST(lock, mutex_unlock_clears_base_lock)
{
	mutex_t m;
	mutex_init(&m);
	mutex_lock(&m);
	mutex_unlock(&m);
	EXPECT_EQ((int)m.base.lock, 0);
	return 0;
}

KTEST(lock, mutex_reacquire)
{
	mutex_t m;
	mutex_init(&m);

	mutex_lock(&m);
	mutex_unlock(&m);
	mutex_lock(&m);
	EXPECT_EQ((int)m.holder, (int)CURRENT_TASK()->psid);
	mutex_unlock(&m);
	EXPECT_EQ((int)m.holder, 0);
	return 0;
}

/* ── rwlock ──────────────────────────────────────────────────────── */

KTEST(lock, rwlock_init_state)
{
	rwlock_t rw;
	rwlock_init(&rw);
	EXPECT_EQ(rw.readers, 0);
	EXPECT_EQ(rw.writer, 0);
	EXPECT_EQ(rw.writers_waiting, 0);
	return 0;
}

KTEST(lock, rwlock_read_lock_increments_readers)
{
	rwlock_t rw;
	rwlock_init(&rw);
	rwlock_read_lock(&rw);
	EXPECT_EQ(rw.readers, 1);
	rwlock_read_unlock(&rw);
	return 0;
}

KTEST(lock, rwlock_read_unlock_decrements_readers)
{
	rwlock_t rw;
	rwlock_init(&rw);
	rwlock_read_lock(&rw);
	rwlock_read_unlock(&rw);
	EXPECT_EQ(rw.readers, 0);
	return 0;
}

KTEST(lock, rwlock_concurrent_readers)
{
	/* Two nested read-locks (no writer): readers must reach 2. */
	rwlock_t rw;
	rwlock_init(&rw);
	rwlock_read_lock(&rw);
	rwlock_read_lock(&rw);
	EXPECT_EQ(rw.readers, 2);
	rwlock_read_unlock(&rw);
	rwlock_read_unlock(&rw);
	EXPECT_EQ(rw.readers, 0);
	return 0;
}

KTEST(lock, rwlock_write_lock_sets_writer)
{
	rwlock_t rw;
	rwlock_init(&rw);
	rwlock_write_lock(&rw);
	EXPECT_EQ(rw.writer, 1);
	rwlock_write_unlock(&rw);
	return 0;
}

KTEST(lock, rwlock_write_unlock_clears_writer)
{
	rwlock_t rw;
	rwlock_init(&rw);
	rwlock_write_lock(&rw);
	rwlock_write_unlock(&rw);
	EXPECT_EQ(rw.writer, 0);
	return 0;
}

KTEST(lock, rwlock_write_reacquire)
{
	rwlock_t rw;
	rwlock_init(&rw);
	rwlock_write_lock(&rw);
	rwlock_write_unlock(&rw);
	rwlock_write_lock(&rw);
	EXPECT_EQ(rw.writer, 1);
	rwlock_write_unlock(&rw);
	EXPECT_EQ(rw.writer, 0);
	return 0;
}

KTEST(lock, rwlock_read_then_write)
{
	rwlock_t rw;
	rwlock_init(&rw);

	rwlock_read_lock(&rw);
	EXPECT_EQ(rw.readers, 1);
	EXPECT_EQ(rw.writer, 0);
	rwlock_read_unlock(&rw);

	rwlock_write_lock(&rw);
	EXPECT_EQ(rw.readers, 0);
	EXPECT_EQ(rw.writer, 1);
	rwlock_write_unlock(&rw);

	EXPECT_EQ(rw.readers, 0);
	EXPECT_EQ(rw.writer, 0);
	return 0;
}

/* ── cond ────────────────────────────────────────────────────────── */

KTEST(lock, cond_init_free_state)
{
	/* init with 0 means event is already fired (lock==0). */
	cond_t c;
	cond_init(&c, 0);
	EXPECT_EQ((int)c.base.lock, 0);
	return 0;
}

KTEST(lock, cond_init_armed_state)
{
	/* init with 1 means event has not fired yet (lock==1). */
	cond_t c;
	cond_init(&c, 1);
	EXPECT_EQ((int)c.base.lock, 1);
	return 0;
}

KTEST(lock, cond_wait_on_free_event)
{
	/* cond_wait on an already-free event (lock==0) returns immediately
	 * and sets lock to 1 (consumed). */
	cond_t c;
	cond_init(&c, 0);
	cond_wait(&c, 0);
	EXPECT_EQ((int)c.base.lock, 1);
	return 0;
}

KTEST(lock, cond_reset_arms_event)
{
	cond_t c;
	cond_init(&c, 0); /* starts free */
	cond_reset(&c); /* arm it */
	EXPECT_EQ((int)c.base.lock, 1);
	return 0;
}

KTEST(lock, cond_notify_clears_lock)
{
	/* notify on an armed cond clears the lock (fires the event). */
	cond_t c;
	cond_init(&c, 1); /* armed */
	cond_notify(&c);
	EXPECT_EQ((int)c.base.lock, 0);
	return 0;
}

KTEST(lock, cond_notify_then_wait)
{
	/* notify fires event → wait consumes it immediately. */
	cond_t c;
	cond_init(&c, 1); /* armed; wait would block */
	cond_notify(&c); /* fires event: lock→0 */
	cond_wait(&c, 0); /* consumes it: lock→1 */
	EXPECT_EQ((int)c.base.lock, 1);
	return 0;
}
