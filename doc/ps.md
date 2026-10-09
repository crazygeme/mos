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

### Context switch: `_task_sched`

`_task_sched()` disables interrupts and reaps inactive dead threads. It holds
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
void do_exit(unsigned status);
```

1. If psid == 0: `DIE()` (kernel bug).
2. If psid == 1 (init): `shutdown()`.
3. If vfork child: `cond_notify(&vfork_event)` (unblocks parent).
4. Flush dirty file-backed pages (`vm_flush_all_dirty`).
5. Close all open file descriptors.
6. Tear down user address space (`vm_destroy` + unmap page directory).
7. Reparent all children to init (psid 1); send SIGCHLD to init if any are zombies.
8. `ps_put_to_dying_queue(current)` — sets status = `ps_dying`, sends SIGCHLD to parent.
9. `task_sched()` — switch away; this task never runs again.

---

## 8. Wait: `do_waitpid`

```c
int do_waitpid(int pid, unsigned *status, int options, struct rusage *ru);
```

Polling loop (yields CPU between attempts):

```
loop:
  task_sched()           // yield
  scan dying_queue:
    match: parent == current AND (pid==0 OR psid==pid)
      → reap child, copy exit status + rusage, return psid
  if WNOHANG: return 0
  check for signals:
    non-SIGCHLD + unmasked → return -EINTR
  if no children remain: return -ECHILD
  ps_put_to_wait_queue(current)   // block until SIGCHLD wakes us
  goto loop
```

**Reaping** (`ps_reap_task`): records usage, drops resource references, and frees task-private blocks and the kernel stack. Shared resources survive until their final reference is released.

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
  └─ _task_sched → SAVE_ALL(current) → pick next → RESTORE_ALL(next) → jmp

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
  └─ poll dying_queue → reap → free task memory → return exit status

signal delivery (on return to user mode)
  └─ push signal frame on user stack → jmp to handler
  └─ trampoline → sys_sigreturn → restore context
```

CPU usage and reporting interfaces are defined in
[CPU usage accounting](bugfix_journal.md#2026-10-08---cpu-usage-accounting-and-atomic-operation-widths).
