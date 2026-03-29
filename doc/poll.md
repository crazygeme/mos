# Poll / Select — Algorithm

## Overview

`poll(2)` and `select(2)` share the same four-phase loop implemented in
`src/fs/poll.c` (`do_poll`) and `src/fs/select.c` (`do_select`).

The central challenge is the **lost-wakeup** race: an fd can become ready
between the readiness check and the moment the task blocks.  The design
eliminates this with a register-then-recheck sequence before sleeping.

---

## The Four-Phase Loop

```
loop:
  Phase 1 — poll_check / select_check
    Query .poll on each fd.
    If any fd is ready → return immediately.
    If timeout == 0     → return 0 (non-blocking test done).

  Phase 2 — poll_reg / select_reg
    For each fd with a .poll_wait hook:
      call poll_wait(fp, cur_task)  — stores task pointer in the fd's wait slot.
    If any fd has no .poll_wait (unsupported):
      set has_unsupported = 1.

  Phase 3 — re-check (closes the lost-wakeup window)
    Query .poll again on each fd.
    If any fd ready → poll_dereg then return.

  Phase 4 — sleep
    Compute sleep_ms:
      infinite + all supported → sleep_ms = 0 (sleep until woken)
      finite timeout           → sleep_ms = deadline - now
      has_unsupported          → cap sleep_ms at TICK_MS (10 ms fallback poll)
    time_wait(sleep_ms)
    poll_dereg (remove task pointer from every fd's wait slot)
    If signal pending → return -EINTR.
  goto loop
```

The loop terminates when:
- at least one fd is ready (return count / fd_set),
- the deadline has passed (return 0), or
- a signal arrives (return -EINTR).

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
If data is found in phase 3, `poll_dereg` runs before returning so no
stale task pointer remains in the fd's wait slot.

---

## fd Driver Interface

Each `file_operations` struct may provide three hooks:

```c
int  (*poll)            (file *fp, unsigned type);
void (*poll_wait)       (file *fp, task_struct *task);
void (*poll_wait_remove)(file *fp, task_struct *task);
```

| Hook               | Called by             | Purpose                                                                |
| ------------------ | --------------------- | ---------------------------------------------------------------------- |
| `poll`             | `fs_select(fd, type)` | Returns 0 if ready, -1 if not                                          |
| `poll_wait`        | Phase 2               | Store `task` so the driver can call `ps_put_to_ready_queue` when ready |
| `poll_wait_remove` | Phase 4 / early exit  | Clear the stored pointer; prevents stale wakeups                       |

`type` is one of `FS_POLL_READ` (0), `FS_POLL_WRITE` (1), `FS_POLL_EXCEPT` (2).

---

## Socket Implementation

Sockets (`src/net/sock.c`) store one task pointer per socket:

```c
mos_sock.poll_task   /* set by sock_poll_wait, cleared by sock_poll_wait_remove */
```

`sock_wakeup()` (called from lwIP callbacks in DSR context) wakes both:
- `sk->waiter` — the task blocked in a blocking `read`/`write`, and
- `sk->poll_task` — the task blocked in `poll`/`select`.

This means a socket can simultaneously serve one blocking I/O caller and
one `poll`/`select` caller without conflict.

---

## Unsupported fds (Fallback Timer)

Some file types (e.g. regular files, `/dev` devices without a `poll_wait`
hook) do not implement `poll_wait`.  For these, the task cannot be woken
by the driver.  Instead:

- `has_unsupported = 1` is set during phase 2.
- The sleep in phase 4 is capped at `TICK_MS` (= 1000/HZ ms, currently 10 ms).
- The loop re-runs at most every 10 ms to re-poll those fds.

Regular files always report ready in phase 1, so in practice this fallback
only triggers for unusual device nodes.

---

## select(2) Extras

`do_select` has two additions over `do_poll`:

1. **Input set snapshot** — the kernel copies `readfds`/`writefds`/`exceptfds`
   into private `reads`/`writes`/`excepts` at entry (heap-allocated).  This
   preserves the original sets across iterations, because `select_check` zeroes
   and refills the output sets on each call.

2. **Signal mask swap (`pselect6`)** — if a `sigmask` argument is supplied,
   `do_select` atomically replaces `cur->signal->sig_mask` for the duration of
   the wait and restores it on exit.  This is the `pselect(2)` semantic.
