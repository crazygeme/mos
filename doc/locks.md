# Kernel Locking Primitives

All four primitives are declared in `include/lib/lock.h` and implemented in `src/lib/lock.c`.

---

## 1. Spinlock (`spinlock_t`)

A raw test-and-set spinlock that **disables local interrupts** while held.

### When to use

Use when the critical section is short (a handful of instructions) and sleeping is not an option — typically inside interrupt handlers or when protecting a single field.
Do **not** use for long critical sections; busy-waiting wastes CPU cycles.

### API

```c
spinlock_t lock;
spinlock_init(&lock);       // must be called before first use
spinlock_lock(&lock);       // acquire; disables interrupts, records holder
spinlock_unlock(&lock);     // release; restores interrupt level
spinlock_uninit(&lock);     // tear down (e.g. on device removal)
```

`spinlock_lock` is a macro that forwards to `_spinlock_lock` and automatically passes `__func__` as the holder name for debugging.

### Behaviour

- Uses `__sync_lock_test_and_set` (atomic XCHG) for acquire and `__sync_lock_release` for release.
- Fast path: a single TAS that succeeds immediately, no `PAUSE`.
- Slow path: spins with `PAUSE` (x86 `rep nop`) to reduce memory-bus contention.
- On lock: saves the current interrupt-enable flag in `lock->old_int` via `sched_disable()`.
- On unlock: restores `old_int` via `sched_set_level()`, then clears the lock word.

### Rules

- Never call any sleeping primitive (`mutex_lock`, `cond_wait`, `kmalloc`, …) while holding a spinlock.
- A single spinlock instance must be acquired and released on the **same CPU** (trivially true on UP).
- Nesting two different spinlocks is allowed but must always be acquired in a consistent order to avoid deadlock.

---

## 2. Condition variable (`cond_t`)

A **binary event** (not a counting semaphore). The event starts in one of two states:

| `initstat` passed to `cond_init` | Meaning |
|---|---|
| `0` (available) | `cond_wait` returns immediately on first call |
| `1` (taken) | `cond_wait` blocks until `cond_notify` fires |

### When to use

Signalling between a producer and a consumer where the consumer blocks until the producer posts an event (e.g. keyboard input, pipe data, timer expiry).

### API

```c
cond_t cv;
cond_init(&cv, 1);          // 1 = start blocked

// ── consumer (may sleep) ──────────────────────────────
int r = cond_wait(&cv, 0);  // block; 0 = not interruptible
int r = cond_wait(&cv, 1);  // block; returns -1 if signal pending
cond_reset(&cv);            // re-arm after consuming the event

// ── producer (process or interrupt context) ───────────
cond_notify(&cv);           // fire event + wake one waiter + sched()
cond_notify_at_intr(&cv);   // fire event only (no sleep, no sched)
cond_wait_at_intr(&cv);     // poll-wait (no sleep) for interrupt ctx
```

`cond_wait` is a macro that passes `__func__` to the underlying implementation.

### Behaviour

- Internally a `lock_base` with a TAS word and a wait queue.
- **Lost-wakeup prevention**: before sleeping, the task re-checks the lock word while holding `wait_lock` (the same lock held by `cond_notify`). This closes the TOCTOU window between "lock is taken" and "enqueue self".
- `cond_notify` calls `task_sched()` after waking to give the woken task CPU time without waiting for the next timer tick.
- `cond_notify_at_intr` / `cond_wait_at_intr` are for interrupt handlers where calling `task_sched()` to sleep is forbidden.

### Rules

- `cond_reset` must be called by the consumer before the next wait if the same event object is reused for multiple cycles.
- Do not call `cond_wait` from interrupt context; use `cond_wait_at_intr` instead.
- `cond_notify` internally acquires the spinlock `wait_lock` — do not hold an outer spinlock that `cond_notify` could also try to acquire.

---

## 3. Mutex (`mutex_t`)

A **non-recursive, sleeping mutual-exclusion lock**. Only the task that acquired the lock may release it.

### When to use

Protecting a data structure across a section that may block (e.g. file system operations, network I/O). For non-blocking sections a spinlock is lighter-weight.

### API

```c
mutex_t m;
mutex_init(&m);     // must be called before first use

mutex_lock(&m);     // acquire; sleeps if contended
mutex_unlock(&m);   // release; DIE() if called by non-holder
```

`mutex_lock` is a macro that forwards to `_mutex_lock` and passes `__func__`.

### Behaviour

- Built on `lock_base` (same TAS word + wait queue as `cond_t`).
- Records the `psid` of the holding task in `m->holder`; `mutex_unlock` asserts ownership and calls `DIE()` on violation.
- Release is atomic under `wait_lock`: the lock word is cleared and one waiter is woken in a single critical section, preventing lost wakeups.

### Rules

- Non-recursive: a task must not call `mutex_lock` again while already holding the same mutex (deadlock).
- Do not call from interrupt context (may sleep).
- Only the holder may call `mutex_unlock`.

---

## 4. Readers-writer lock (`rwlock_t`)

Allows **multiple concurrent readers** or **one exclusive writer**. Uses a **write-preferring** policy: once a writer is waiting, new readers queue behind it, preventing writer starvation.

### When to use

Protecting data that is read frequently but written rarely (e.g. routing tables, file-system metadata, VFS mount table).

### API

```c
rwlock_t rw;
rwlock_init(&rw);

// ── reader ────────────────────────────────────────────
rwlock_read_lock(&rw);    // acquire shared; blocks while writer holds or waits
rwlock_read_unlock(&rw);  // release shared

// ── writer ────────────────────────────────────────────
rwlock_write_lock(&rw);   // acquire exclusive; blocks until no readers/writers
rwlock_write_unlock(&rw); // release exclusive
```

Both `rwlock_read_lock` and `rwlock_write_lock` are macros that pass `__func__`.

### Behaviour

- All state (`readers`, `writer`, `writers_waiting`, two wait lists) is protected by an internal `spinlock_t wait_lock`.
- **Read lock**: blocked if `writer == 1` or `writers_waiting > 0` (write-preferring).
- **Write lock**: increments `writers_waiting`, then blocks until `readers == 0 && writer == 0`.
- **Read unlock**: if `readers` drops to 0 and a writer is waiting, wakes one writer.
- **Write unlock**: if writers are waiting, wakes one writer; otherwise wakes **all** queued readers so they can proceed concurrently.
- `rwlock_write_unlock` calls `task_sched()` to immediately yield to woken tasks.

### Rules

- Do not upgrade a read lock to a write lock (deadlock: you hold a read lock while waiting for all readers to drain).
- Do not call from interrupt context (may sleep).
- Consistent lock ordering must be enforced across the codebase to prevent deadlock between multiple rwlocks or between rwlock and mutex/spinlock.

---

## Quick-reference table

| Primitive | Sleeps? | Interrupt-safe? | Recursive? | Multiple holders? |
|---|---|---|---|---|
| `spinlock_t` | No (busy-wait) | Yes | No | No |
| `cond_t` | Yes (`_at_intr` no) | `_at_intr` only | N/A | No |
| `mutex_t` | Yes | No | No | No |
| `rwlock_t` | Yes | No | No | Yes (readers) |

## Internal building block: `lock_base`

`cond_t` and `mutex_t` both embed a `lock_base`:

```c
typedef struct _lock_base {
    unsigned int lock;       // 0 = available, 1 = taken (TAS word)
    list_entry   wait_list;  // sleeping task_struct entries
    spinlock_t   wait_lock;  // guards wait_list and lock transitions
} lock_base;
```

The lost-wakeup invariant is maintained by always clearing `lock` **and** waking a waiter in the same `wait_lock` critical section.
