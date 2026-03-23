# Process Management & Scheduler

**Source:** `src/ps/ps.c`, `src/ps/ps_sched.c`, `src/ps/ps_syscall.c`
**Headers:** `include/ps/ps.h`, `include/ps/signal.h`, `src/ps/ps_internal.h`

---

## Overview

| Layer | File | Responsibility |
|-------|------|---------------|
| Task management | `ps.c` | Create/destroy tasks, queue management, PID lookup |
| Scheduler | `ps_sched.c` | Context switch, MPRQ, sleep/wake |
| Syscalls | `ps_syscall.c` | `fork`, `vfork`, `exit`, `waitpid`, signal delivery |

---

## 1. Task structure

Every task (kernel thread or user process) lives in a single `KERNEL_TASK_SIZE`-aligned page:

```
┌──────────────────────────────┐  page base  (task_struct *)
│  task_struct                 │
│    tss  (register save area) │
│    cr3, psid, priority, ...  │
│    signal_context *          │
│    user_enviroment *         │
├──────────────────────────────┤
│  kernel stack (grows down)   │
└──────────────────────────────┘  page top  = initial esp0
```

`CURRENT_TASK()` derives the pointer by masking the low bits of ESP — no per-CPU variable needed.

### `task_struct` fields

```c
typedef struct _task_struct {
    task_frame tss;              // CPU register save area (see below)
    unsigned long cr3;           // page directory base
    unsigned int psid;           // process ID
    process_fn fn;               // kernel entry function
    void *param;                 // parameter for entry function
    user_enviroment *user;       // user-mode context (argv, env, cwd, …)
    signal_context *signal;      // signal handlers + pending/masked sets
    int priority;                // ps_idle=0 | ps_normal=1
    int type;                    // ps_kernel | ps_user
    list_entry ps_list;          // node in ready / wait / dying queue
    struct rb_node mgr_rb;       // node in mgr_queue RB-tree (keyed by psid)
    ps_status status;            // ps_ready | ps_running | ps_waiting | ps_dying
    const char *wait_func;       // debugging: name of blocking call
    int remain_ticks;            // time-slice counter (reset to DEFAULT_TICKS on each schedule)
    unsigned timeout;            // wake-up time for timed sleep (ms)
    file_descriptor *fds;        // open file descriptors
    mutex_t fd_lock;
    unsigned exit_status;        // status word for waitpid
    task_struct *parent;
    unsigned nchildren;          // count of unreapped children
    unsigned fork_flag;          // FORK_FLAG_VFORK
    cond_t vfork_event;          // blocks parent during vfork
    super_block *root;           // root filesystem
    unsigned umask;
    unsigned niv_switches;       // involuntary context switches
    unsigned total_switches;
    unsigned user_tickets;       // user-mode time (10 ms units)
    unsigned kernel_tickets;     // kernel-mode time (10 ms units)
    unsigned pf_major;
    unsigned pf_minor;
    unsigned long long alarm_expire_ms;
    unsigned int magic;          // 0xDEADBEEF — stack overflow sentinel
} task_struct;
```

### Register save area (`task_frame`)

Holds the full x86 user/kernel context:

```
eax ebx ecx edx edi esi ebp esp   — general purpose
ds  ss  es  gs  fs  cs            — segment registers
eip eflags                        — instruction pointer + flags
esp0                              — kernel stack top (written to TSS)
```

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

| Lock | Protects |
|------|---------|
| `ps_lock` (spinlock) | ready / wait / dying / mgr queues |
| `map_lock` (spinlock) | per-process page-table operations |
| `psid_lock` (spinlock) | PID counter |

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

```
_task_sched(func_name)
  1. disable interrupts
  2. drain pending DSR (deferred interrupt handlers)
  3. ps_get_next_task → next
  4. if next == current: re-enable + return   (no-op)
  5. SAVE_ALL(current, NEXT_LABEL)            // saves registers + resume eip
  6. RESTORE_ALL(next)                        // loads next task's registers
  7. sync kernel PDEs into next task's page directory
  8. update TSS esp0 + CR3 for next task
  9. JUMP_TO_NEXT_TASK_EIP(next)              // indirect jmp to saved eip
  -- next task runs from its saved eip --
  NEXT_LABEL:
  10. re-enable interrupts                    // resumed here when rescheduled
```

Macros in `include/macro.h`:

| Macro | Effect |
|-------|--------|
| `SAVE_ALL(task, label)` | Stores all registers to `task->tss`; sets `tss.eip = &label` |
| `RESTORE_ALL(task)` | Loads all registers from `task->tss` |
| `JUMP_TO_NEXT_TASK_EIP(eip)` | `jmp *eip` — indirect jump to resume point |

### Time slices

Each task gets `DEFAULT_TICKS` ticks per schedule. The PIT interrupt decrements `remain_ticks`; when it hits 0 the timer ISR calls `task_sched()` (involuntary preemption).

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

| Transition | Function |
|-----------|---------|
| → ready | `ps_put_to_ready_queue` |
| → waiting | `ps_put_to_wait_queue` |
| → dying | `ps_put_to_dying_queue` (sets status, sends SIGCHLD to parent) |
| waiting → ready (lock released) | caller invokes `ps_put_to_ready_queue` |
| waiting → ready (timeout) | scheduler skips sleeping tasks until `timeout <= now` |

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
int do_fork(int flag);   // flag = 0 (fork) or FORK_FLAG_VFORK (vfork)
```

Steps:

1. Allocate and `memcpy` current `task_struct` into child.
2. Assign new psid; clear per-task stats.
3. **Duplicate user address space** (CoW):
   - Allocate a new page directory for the child.
   - For every mapped user page:
     - Clear `PAGE_ENTRY_WRITABLE` in **both** parent and child PTEs.
     - Map the same physical page into child (increment `ref_count`).
   - First write fault in either process triggers `pf_handle_cow` (see `doc/mm_virtual.md`).
4. Duplicate `fds` array (increment file `ref_count` for each open fd).
5. Duplicate `signal_context`; clear pending signals in child.
6. Set `child->parent = current`; increment `current->nchildren`.
7. Set child `tss.eax = 0` (child gets 0 from fork).
8. Set child `tss.eip = ret_from_fork`.
9. Enqueue child in `ready_queue` + `mgr_queue`.
10. **vfork**: if `FORK_FLAG_VFORK`, parent calls `cond_wait(&child->vfork_event)` and blocks until child calls `exec` or `exit`.
11. Return child psid to parent.

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

**Reaping** (`ps_reap_task`): copies rusage, frees command/env/cwd, page directory, `user_enviroment`, `signal_context`, `task_struct` page.

---

## 9. Signal handling

### Data structures

```c
typedef struct _signal_context {
    struct sigaction sig_handlers[NSIG];  // per-signal handler + flags
    sigset_t sig_pending;                 // bitmask: bit N-1 = signal N pending
    sigset_t sig_mask;                    // bitmask: blocked signals
} signal_context;
```

Standard POSIX signals (1–31). `NSIG = 32`.

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
