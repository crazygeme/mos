# Poll / Select — Algorithm

## Overview

`poll(2)` and `select(2)` share the same four-phase loop implemented in
`src/fs/poll.c` (`poll_wait_loop`).  Both supply a `poll_ops` vtable so the
loop is fd-type-agnostic.

The central challenge is the **lost-wakeup** race: an fd can become ready
between the readiness check and the moment the task blocks.  The design
eliminates this with a register-then-recheck sequence before sleeping.

---

## `poll_ops` — vtable interface

```c
struct poll_ops {
    int  (*check)(void *ctx);  /* inspect all fds; return ready count */
    int  (*reg)  (void *ctx);  /* register task on every fd; return has_unsupported */
    void (*dereg)(void *ctx);  /* remove task registration from every fd */
};
```

`poll.c` and `select.c` each define their own context type and a `poll_ops`
instance.  `poll_wait_loop` calls only through this vtable.

---

## The Four-Phase Loop (`poll_wait_loop`)

```c
int poll_wait_loop(const struct poll_ops *ops, void *ctx,
                   int just_test, int infinite, unsigned deadline);
```

```
loop:
  Phase 1 — ops->check(ctx)
    Query readiness of all fds.
    If any fd is ready (ret > 0) → return ret.
    If just_test (timeout == 0)  → return 0.
    If !infinite && now >= deadline → return 0.

  Phase 2 — has_unsupported = ops->reg(ctx)
    For each fd: call fs_fd_poll(fd, want, poll_table *pt)
      → driver stores current task in pt for wakeup.
    Return 1 if any fd lacks poll_wait support.

  Phase 3 — re-check (closes the lost-wakeup window)
    ops->check(ctx) again.
    If ready → ops->dereg(ctx); return ret.

  Phase 4 — sleep
    Compute sleep_ms:
      infinite + all supported → sleep_ms = 0 (sleep until woken)
      finite timeout           → sleep_ms = deadline - now
      has_unsupported          → cap sleep_ms at TICK_MS (10 ms fallback poll)
    If finite and now >= deadline → ops->dereg(ctx); return 0.
    time_wait(sleep_ms)
    ops->dereg(ctx)
    If signal pending (sig_pending & ~sig_mask) → return -EINTR.
  goto loop
```

The loop terminates when:
- at least one fd is ready → return ready count / filled fd_sets,
- the deadline has passed → return 0, or
- a signal arrives → return -EINTR.

---

## Why Phase 3 Is Necessary

Without the re-check, the following race is possible:

```
task                         driver / lwIP callback
----                         ----------------------
Phase 1: check → not ready
                             data arrives → sock_wakeup() fires
                             (task not yet registered → wakeup lost)
Phase 2: register
Phase 4: sleep forever
```

Phase 3 catches data that arrived in the window between phases 1 and 2.
If data is found in phase 3, `ops->dereg` runs before returning so no
stale task pointer remains in any fd's wait slot.

---

## fd Driver Interface

Each `file_operations` may implement:

```c
unsigned (*poll)(file *fp, unsigned want, poll_table *pt);
```

A single function handles both readiness query and registration:
- If `pt == NULL`: check readiness only; return a bitmask of ready types.
- If `pt != NULL`: also call `poll_table_add(pt, fp)` to store the task for wakeup.

`want` and the return value are bitmasks of:

| Flag            | Value | Meaning           |
| --------------- | ----- | ----------------- |
| `FS_POLL_READ`  | 1     | readable / data   |
| `FS_POLL_WRITE` | 2     | writable          |
| `FS_POLL_EXCEPT`| 4     | exception / error |

`fs_fd_poll(fd, want, pt)` is the VFS wrapper called by both poll and select
contexts.

### `poll_table` — registration table

```c
typedef struct {
    file         *fp;
    task_struct  *task;
} poll_table_entry;

typedef struct {
    task_struct      *task;
    poll_table_entry *entries;
    unsigned          cap;
    unsigned          n;
    int               unsupported;
} poll_table;
```

`poll_table_init(pt, task, entries, cap)` sets up the table with a
pre-allocated entry array (sized `nfds * 2` by the caller).
`poll_table_add(pt, fp)` stores one `(fp, task)` pair.
`poll_table_cleanup(pt)` calls `fp->fops->poll_wait_remove(fp, task)` on
every registered entry, clearing stale task pointers from drivers.

---

## Socket Implementation

Sockets (`src/net/sock.c`) store one task pointer per direction in the
`poll_table`:

`sock_wakeup()` (called from lwIP callbacks in DSR context) wakes both:
- `sk->waiter` — the task blocked in a blocking `read`/`write`, and
- any task registered via the `poll_table` for `poll`/`select`.

---

## Unsupported fds (Fallback Timer)

Some file types (e.g. regular files, `/dev` devices) do not implement
`poll` registration (`poll_wait` support missing).  For these:

- `pt->unsupported` is incremented during phase 2.
- `ops->reg` returns `has_unsupported = 1`.
- The sleep in phase 4 is capped at `TICK_MS` (= 1000/HZ ms, currently 10 ms).
- The loop re-runs every 10 ms to re-poll those fds.

Regular files always report ready in phase 1, so in practice this fallback
only triggers for unusual device nodes.

---

## `poll(2)` Implementation (`do_poll`)

`struct poll_ctx` holds `struct pollfd *fds`, `unsigned nfds`, and a `poll_table`.

The caller pre-allocates `nfds * 2` `poll_table_entry` slots (heap, freed after).

`poll_ctx_check`:
- Translates `POLLIN`/`POLLOUT` → `FS_POLL_READ`/`FS_POLL_WRITE`.
- Fills `fds[i].revents`; returns ready count.

`poll_ctx_reg`:
- Calls `fs_fd_poll(fd, want, pt)` for each fd (registration path).
- Returns `pt->unsupported`.

`poll_ctx_dereg`:
- Calls `poll_table_cleanup(&ctx.wait)`.

---

## `select(2)` / `pselect6(2)` Implementation (`do_select`)

`struct select_ctx` holds:
- Snapshot copies of input sets (`reads`, `writes`, `excepts`) — allocated once and never modified across iterations, because `select_ctx_check` zeroes and refills the caller's output sets on every call.
- Pointers to the caller's output sets (`readfds`, `writefds`, `exceptfds`).

`do_select` additionally:

1. **Signal mask swap (`pselect6`)** — if a `sigmask` argument is non-NULL,
   replaces `cur->signal->sig_mask` for the duration of the wait and restores
   it on exit.  This is the `pselect(2)` / `ppoll(2)` semantic.

2. **`timeout` is `struct timespec *`** (not milliseconds) — converted to an
   absolute `deadline` in ms.  `NULL` → infinite wait.  Both fields zero →
   `just_test = 1` (non-blocking).
