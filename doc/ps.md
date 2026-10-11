# Process Management & Scheduler

**Source:** `src/ps/impl/` and `arch/{x86,x64}/ps/impl/`
**Headers:** `src/ps/ps.h`, `src/ps/clone.h`, `src/lib/ref_count.h`, `src/mm/mmap.h`

---

## Overview

| Layer           | File           | Responsibility                                      |
| --------------- | -------------- | --------------------------------------------------- |
| Task management | `ps.c`         | Create/destroy tasks, queue management, PID lookup  |
| Scheduler       | `ps_sched.c`   | Context switch, MPRQ, sleep/wake                    |
| Syscalls        | `ps_syscall.c` | `fork`, `vfork`, `exit`, `waitpid`, signal delivery |

---

## 1. Task structure and resource ownership

The task header sits at the base of a `KERNEL_TASK_BYTES`-aligned kernel stack
allocation. `CURRENT_TASK()` masks the stack pointer to find it. State blocks
are allocated separately, leaving more space for the kernel call stack.

```c
struct _task_struct {
    task_schedule *sched;
    task_wait *wait;
    task_lifecycle *life;
    task_execution *execution;
    task_credentials *credentials;
    task_signal *signal;
    task_stats_t *stats;
    task_memory *memory;
    task_files *files;
    task_fs *fs;
    signal_handlers *sighand;
    task_thread *thread;
    unsigned magic;
};
```

Each clone flag controls one shared resource independently:

| Flag | Block | Owned state |
| --- | --- | --- |
| `CLONE_VM` | `task_memory` | Mappings, page-table root, heap/stack bounds, image metadata, LDT |
| `CLONE_FILES` | `task_files` | Descriptor table, close-on-exec bitmap, descriptor lock |
| `CLONE_FS` | `task_fs` | Filesystem root, chroot prefix, cwd, umask |
| `CLONE_SIGHAND` | `signal_handlers` | Signal dispositions |
| `CLONE_THREAD` | `task_thread` | TGID, PGID/SID, limits, group CPU totals, process pending signals, ITIMER_REAL |

Without a flag, the child receives an independent copy; a new thread-group
block starts with fresh accounting, pending signals, and timers. Private blocks
are always distinct. In particular, `task_execution` owns FPU/TLS/ptrace state,
I/O permissions, robust-list registration, clear-child-TID state, and the VMA
cache. `task_signal` owns the thread mask, thread pending signals, saved-mask
restoration, and alternate stack.

All shared blocks begin with `ref_count_t ref`, defined in `lib/ref_count.h`.
`ref_count_init`, `ref_count_get`, `ref_count_put`, and `ref_count_read` provide
the common lifetime API; the final put calls the resource's release callback.
`ref_count_get(resource)` returns the same pointer, so cloning can use
`child->sighand = ref_count_get(parent->sighand);`.
There is one VM reference count, and group accounting belongs to `task_thread`.

`ps_clone_resources()` selects owners from the clone flags. Exec detaches the
filesystem and handler blocks as needed, prepares a fresh `task_memory`, then
activates it before dropping the old memory reference. The VMA cache remains
private and validates both memory identity and VMA generation.

Scheduler and wait nodes use `list_entry` or RB-tree nodes embedded in their
blocks, with an owner pointer back to the task. Active I/O scopes use a private
intrusive list under `task_wait`: each scope retains an open description even
if its descriptor closes. They cannot share the descriptor table's lifetime,
because `CLONE_FILES` must not let one thread cancel another thread's transfers.

Architecture switches save registers on the kernel stack and retain the stack
pointer in `task_schedule`. `task_execution.arch` contains only persistent
selector/base state. Initial instruction and stack pointers are passed directly
to frame construction; the kernel stack top is derived from the task allocation.

---

## 2. Scheduler control block

```c
typedef struct _ps_control {
    list_entry ready_queue[PS_PRIORITY_MAX]; // [0]=idle  [1]=normal
    list_entry dying_queue;                  // zombies awaiting reap
    list_entry wait_queue;                   // blocked on locks/waitpid
    struct rb_root mgr_queue;               // all tasks, keyed by psid
    int ps_count;
} ps_control;
```

Global locks:

| Lock                   | Protects                          |
| ---------------------- | --------------------------------- |
| `ps_lock` (spinlock)   | ready / wait / dying / mgr queues |
| `map_lock` (spinlock)  | per-process page-table operations |
| `psid_lock` (spinlock) | PID counter                       |

---

## 3. Scheduler algorithm (MPRQ)

The scheduler is a **Multilevel Priority Ready Queue** with two levels:

```
priority 1 (normal):  [task A] ↔ [task B] ↔ [task C] ↔ …
priority 0 (idle):    [idle]
```

### `ps_get_next_task`

Scans `ready_queue[]` from highest priority down:

1. For each priority level, walk the list.
2. Skip `ps_dying` tasks.
3. Skip tasks with `timeout > now_ms()` (still sleeping).
4. First suitable task wins — moved to **tail** of its queue (round-robin fairness).
5. Falls back to `ps_idle` if nothing else is runnable.

### Context switch: `task_sched`

`task_sched()` disables interrupts and reaps inactive dead threads. It holds
`ps_lock` while selecting and activating a runnable task. The current task remains runnable
unless a caller has explicitly placed it in a waiting or terminal state.
The switch updates address-space, segment, FPU, and CPU ownership state;
`ps_context_switch()` saves and restores the kernel call stack. Assembly calls
`ps_switch_finish()` on the incoming stack to clear outgoing CPU ownership,
wake a waiting zombie parent, and release `ps_lock`. Each task
restores its own interrupt state when its scheduler call returns.

Deferred device callbacks execute on the `dsr_worker` kernel task at
`ps_deferred` priority, above `ps_normal` and `ps_idle`. Callbacks may yield
or wait using their own task state. The worker sleeps when its queue is
empty. Queue insertion wakes that empty-queue wait under the DSR lock;
it does not interrupt a wait taken inside a callback. Interrupt exit
requests scheduling when the worker is ready and scheduling is enabled.

### Time slices

`DEFAULT_TASK_TIME_SLICE` is 500 ms, or 50 ticks at `HZ = 100`. Timer
interrupts decrement `remain_ticks`. An expired slice triggers scheduling
and replenishes the slice. Scheduling for deferred work preserves the
interrupted task's remaining slice.

---

## 4. Task states

```
ps_create()
    │
    ▼
 ps_ready ──── schedule ────► ps_running
    ▲                              │
    │    wake / timeout            ▼
    └────────────────────── ps_waiting
                                   │ (exit/do_exit)
                                   ▼
                              ps_dying ──── reap ───► freed
```

State transitions:

| Transition                      | Function                                                       |
| ------------------------------- | -------------------------------------------------------------- |
| → ready                         | `ps_put_to_ready_queue`                                        |
| → waiting                       | `ps_put_to_wait_queue`                                         |
| → dying                         | `ps_put_to_dying_queue` (sets status, sends SIGCHLD to parent) |
| waiting → ready (lock released) | caller invokes `ps_put_to_ready_queue`                         |
| waiting → ready (timeout)       | scheduler skips sleeping tasks until `timeout <= now`          |

---

## 5. Task creation: `ps_create`

```c
task_struct *ps_create(process_fn fn, void *param, int priority, int type);
```

1. Allocate one `KERNEL_TASK_SIZE`-aligned page for the task struct + kernel stack.
2. Initialise fields: psid (atomic counter), fn, param, priority, type.
3. Set `tss.eip = ps_run` — the trampoline that calls `fn(param)`.
4. Set `tss.esp0 = page_top`.
5. Insert into `mgr_queue` (RB-tree) and `ready_queue`.

`ps_run` calls `fn(param)`, then calls `do_exit(0)` if fn returns.

---

## 6. Fork: `do_fork`

```c
static int do_fork(void);
int do_vfork(unsigned long child_stack, unsigned long flags);
```

Steps:

1. Allocate fresh task-private blocks and a child PID.
2. Use `ps_clone_resources()` to select independent resource owners.
3. For a new VM, copy metadata and establish copy-on-write user mappings.
4. Copy credentials, FPU/TLS state, and the inherited signal mask. Reset
   transient waits, pending thread signals, robust registration, and clear-child-TID.
5. Build the child return frame with a zero syscall result.
6. Add the child to the ready and management queues.
7. For vfork, block the parent on the child's event until exec or exit.

---

## 7. Exit: `do_exit`

```c
void do_exit(unsigned encoded_status);
```

`task_thread` owns a member list, a child-process list, and a count of living
members. Each child records the parent TGID in `task_lifecycle.ppid` and links
into the parent's group child list. A separate creator list supports
`PR_SET_PDEATHSIG` for the thread that created the child.

Exit cancels I/O waits, clears the registered child TID, releases robust futexes,
flushes dirty shared mappings, and drops file-table references. Under `ps_lock`,
`ps_mark_dying_unsafe()` marks the task dead, updates the living-member count,
and processes creator-death signals. An ordinary thread exit preserves each
child's PPid. The final living member transfers the group's remaining children
to PID 1 and moves waitable zombies into PID 1's exit queue. Notification and
queue updates occur within the same critical section.

`exit_group` enumerates the group's member list, stops sibling tasks, and waits
for remote execution and VM transactions to finish before releasing their
resources. The process leader remains registered with its PID and encoded exit
status until reaping. A leader that exits while sibling threads remain alive
becomes waitable only after the final sibling exits.

Exited ordinary threads enter a deferred cleanup queue. Their kernel stacks
are released after they leave all CPUs and process-enumeration references have
been dropped. A waitable process retains its resource references until
`ps_reap_task()` records usage and releases the task. PID 1 exit invokes system
shutdown; test mode exits QEMU with the encoded test status.

---

## 8. Wait: `do_waitpid`

```c
int do_waitpid(unsigned pid, int *status, int options, rusage *usage);
int do_waitpid_pgrp(unsigned pgrp, int *status, int options, rusage *usage);
```

Any member of a parent thread group may wait for its child processes. Thread
members are excluded from the child-process list. PID selection uses the
management tree for ownership checks; exit and process-group selection inspect
only the group's child list.

```
loop with ps_lock held:
  scan the group child list for a matching waitable process:
    require exit_ready, no executing CPU, and no living group members
    detach the child, preserve status, release the lock, reap, return PID
  report a matching pending stop when WUNTRACED or ptrace permits it
  if no matching child exists: return -ECHILD
  if an unmasked non-SIGCHLD signal interrupts the wait: return -EINTR
  if WNOHANG: return 0 without modifying status or reaping another child
  enqueue the caller as an interruptible waiter and release ps_lock
  schedule and repeat
```

Exit notification wakes waiting members of the parent group, including waiters
that block or ignore `SIGCHLD`. A task leaving its CPU also wakes the parent
waiters so a child observed on a CPU can subsequently be reaped. Resource
references shared with another task remain valid until their final release.

`ProcessLifecycle` kernel tests cover ownership, member counts, creator-death
signals, atomic final-exit adoption, wait selection, and PID retention.
`test/thread_group_wait.sh` validates these operations with running clone
threads, process-group waits, competing waiters, fatal signals, orphan reaping,
and shared-pipe closure. Both suites are registered by the standard
`./run.sh kvm test` workflow.

---

## 9. Signal handling

### Data structures

`signal_handlers` is shared according to `CLONE_SIGHAND`; `task_signal` is
private. `task_thread` holds process-directed pending signals and timer events.
Delivery consumes one signal from the union of group and thread pending sets
under `ps_lock`. Timer payloads use `list_entry` queues so different timers do
not overwrite one shared payload slot.

Standard signals are 1–31; the supported timer signal is 32 (`NSIG = 33`).

### Delivery

Signals are checked on every return to user mode:

1. Compute `deliverable = sig_pending & ~sig_mask`.
2. For each pending deliverable signal:
   - If `SIG_DFL`: apply default action (usually SIGKILL → `do_exit`, SIGCHLD → ignore).
   - If `SIG_IGN`: clear bit, continue.
   - Otherwise: set up a **signal frame** on the user stack and transfer control to the handler.

### Signal frame layout (on user stack)

```
[esp+ 0]  return address → trampoline ("mov $119,%eax; int $0x80")
[esp+ 4]  signo
[esp+ 8]  saved_eip
[esp+12]  saved_eflags
[esp+16]  saved_esp
[esp+20]  saved_eax … saved_ebp    (all GP registers)
[esp+48]  saved_mask
[esp+52]  trampoline code (8 bytes, mapped in the frame)
```

On return from the handler, the trampoline calls `sys_sigreturn` (syscall 119), which pops the saved context off the user stack and restores `eip`, `eflags`, `esp`, and all GP registers.

---

## 10. Synchronization primitives

### Spinlock (`spinlock_t`)

Test-and-set spin lock. Disables local interrupts while held to prevent self-deadlock. Contending callers spin using `PAUSE()` (x86 `pause` instruction).

### Mutex (`mutex_t`)

Binary sleeping lock. Tracks holder psid. Built on `lock_base` (semaphore + wait list). Only the holder may unlock; checked with `DIE()`.

### Condition variable (`cond_t`)

Binary event. `cond_wait(cond, interruptible)` blocks until `cond_notify(cond)` fires or (if interruptible) a signal arrives. Used for vfork parent blocking.

### Reader-writer lock (`rwlock_t`)

Write-preferring: once a writer is waiting, new readers are blocked. Multiple readers hold concurrently; writers get exclusive access.

---

## 11. Lifecycle summary

```
ps_create(fn, param)
  └─ alloc 1 page, init task_struct, tss.eip = ps_run
  └─ insert mgr_queue + ready_queue

PIT interrupt (every 10 ms)
  └─ decrement remain_ticks
  └─ remain_ticks == 0 → task_sched()

task_sched()
  └─ task_sched → SAVE_ALL(current) → pick next → RESTORE_ALL(next) → jmp

fork()
  └─ do_fork → CoW duplicate address space + fds + signal context
  └─ child resumes at ret_from_fork with eax=0

exec()
  └─ load ELF, replace address space, reset signal handlers
  └─ if vfork child: cond_notify(vfork_event) → unblock parent

exit(status)
  └─ do_exit → flush pages → close fds → vm_destroy
  └─ ps_put_to_dying_queue → SIGCHLD to parent → task_sched()

waitpid(pid)
  └─ scan group children → reap → free task memory → return exit status

signal delivery (on return to user mode)
  └─ push signal frame on user stack → jmp to handler
  └─ trampoline → sys_sigreturn → restore context
```

CPU usage and reporting interfaces are defined in
[CPU usage accounting](bugfix_journal.md#2026-10-08---cpu-usage-accounting-and-atomic-operation-widths).
