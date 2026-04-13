# Signal Handling

**Source:** `src/ps/ps_signal.c`, `src/ps/ps.c`, `src/ps/ps_sched.c`, `src/syscall/syscall_proc.c`, `include/ps/signal.h`

## Status

MOS currently supports the Linux/i386-style signal model needed by the RH9/glibc userspace in this tree:

- classic `signal(2)` and `sigaction(2)`
- `rt_sigaction(2)` with the Linux/i386 userspace layout
- `sigprocmask` and `rt_sigprocmask`
- inline on-stack `sigreturn` and `rt_sigreturn` trampolines
- `kill`, `pause`, `sigaltstack`
- `rt_sigpending`, `rt_sigtimedwait`, `rt_sigqueueinfo`, `rt_sigsuspend`
- `alarm`, `setitimer(ITIMER_REAL)`, and `getitimer(ITIMER_REAL)`

Delivery is process-local and bitmask-based: standard signals are pending or not pending, with no queued multiplicity and no queued `siginfo` payloads.

## Per-Task Signal State

Each user task owns a `signal_context`:

```c
typedef struct _signal_context {
    struct sigaction sig_handlers[NSIG];
    sigset_t sig_pending;
    sigset_t sig_mask;
    sigset_t saved_sigmask;
    int restore_sigmask;
    stack_t altstack;
} signal_context;
```

Important points:

- `sig_pending` is a bitmask, not a queue.
- `sig_mask` is the currently blocked set.
- `saved_sigmask` and `restore_sigmask` are used by `rt_sigsuspend`.
- `altstack` tracks `sigaltstack(2)` state, including `SS_ONSTACK`.

`sigset_t` is effectively a 32-bit low-word mask in the current implementation.

## Signal Numbers and Default Actions

The tree uses Linux/i386 signal numbers `1..31` with `NSIG = 32`.

Important defaults in the current code:

- `SIGKILL` and `SIGSTOP`: cannot be caught, ignored, or blocked
- `SIGCHLD`, `SIGURG`, `SIGWINCH`, `SIGCONT`: default action is ignore
- most others: default action is terminate

For `SIGKILL` and `SIGSTOP`, the kernel exits immediately with an encoded wait status path instead of building a signal frame.

## Sending Signals

### `ps_send_signal`

`ps_send_signal(pid, sig)`:

- validates the signal number
- finds the target task
- checks permission with Unix-style uid/euid rules
- sets the pending bit
- wakes the target only if it is sleeping and the signal is not masked

That last rule is important for `rt_sigsuspend`: a masked signal should remain pending without spuriously waking the waiter.

### `kill(2)`

`sys_kill` currently supports:

- `pid > 0`: one process
- `pid == 0`: caller's process group
- `pid == -1`: every other user process
- `pid < -1`: process group `-pid`
- `sig == 0`: existence and permission check only

Self-signal special case:

- if a process sends an unmasked signal to itself, MOS forces delivery immediately on the current user-return frame by calling `do_signal()` directly

That makes shell trap/self-signal behavior much closer to Linux.

## Where Delivery Happens

`do_signal(intr_frame *frame)` is the delivery engine.

It runs only when returning to user mode:

- at the end of the syscall path
- from the interrupt-return path for user tasks

The function returns immediately if:

- the current task is not a user task
- the interrupted frame is not returning to ring 3
- no unmasked signal is pending

## Delivery Algorithm

Current delivery sequence:

1. Check whether `alarm_expire_ms` has elapsed; if so, set `SIGALRM` pending and optionally re-arm for interval timers.
2. Compute `deliverable = sig_pending & ~sig_mask`.
3. Pick the lowest-numbered set bit.
4. Clear that pending bit.
5. Dispatch according to the installed disposition.

Disposition handling:

- `SIGKILL` / `SIGSTOP`: force immediate exit
- `SIG_IGN`: consume and return
- `SIG_DFL`: either ignore or terminate depending on the signal
- user handler: build a signal frame, update masks, redirect `eip` and `esp`

MOS chooses the RT frame path only when `SA_SIGINFO` is set. Installing a handler through `rt_sigaction` alone does not force the RT frame format.

## Signal Masks

### `sigprocmask` and `rt_sigprocmask`

Both variants support:

- `SIG_BLOCK`
- `SIG_UNBLOCK`
- `SIG_SETMASK`

Current behavior:

- `SIGKILL` and `SIGSTOP` are always stripped from the blocked set
- classic `sigprocmask` reads and writes one machine word
- `rt_sigprocmask` expects an 8-byte Linux/i386 kernel mask payload and only uses the low 32 bits

## Legacy and RT Handlers

MOS supports two delivery frame formats.

### Legacy frame

Used when `SA_SIGINFO` is clear.

The kernel writes a `signal_frame` onto the user stack containing:

- handler argument `signo`
- saved general-purpose registers
- saved segment registers
- saved `eip`, `eflags`, `esp`
- saved signal mask
- an inline `sigreturn` trampoline

The trampoline bytes are written directly into the user stack frame:

```text
mov $__NR_sigreturn, %eax
int $0x80
nop
```

The handler returns with a normal `ret`, which jumps into that inline stub.

### RT frame

Used when `SA_SIGINFO` is set.

The RT path builds a larger Linux/i386-compatible frame containing:

- signal number
- `siginfo` storage
- `ucontext`
- zeroed FP-state area
- either `sa_restorer` or an inline `rt_sigreturn` stub

Current limitation:

- MOS does not queue full `siginfo` objects for pending signals
- `rt_sigqueueinfo` sends the signal but does not preserve a rich queued payload

## Signal Stack Selection

`resolve_sigstack()` chooses the delivery stack:

- if `SA_ONSTACK` is set and a usable altstack is installed and not already active, deliver on the altstack and set `SS_ONSTACK`
- otherwise deliver on the current user stack

Frames are then rounded down to a 16-byte boundary before control is transferred to the handler.

## Returning From a Handler

### `sys_sigreturn`

The legacy return path:

- finds the `signal_frame` below the current user `esp`
- restores saved registers into the kernel interrupt frame
- restores the saved signal mask
- clears `SS_ONSTACK` if the restored `esp` has left the altstack
- returns the interrupted `eax`

### `sys_rt_sigreturn`

The RT return path performs the same job using the saved `ucontext`.

In both cases, the interrupted CPU state is restored into the kernel's pending return frame, so execution resumes exactly where it left off.

## `sigaction` and `signal`

### `sigaction`

`sys_sigaction` directly copies the kernel `struct sigaction` layout in and out.

Rules:

- invalid signal numbers are rejected
- `SIGKILL` and `SIGSTOP` cannot be changed

### `signal`

`sys_signal` is implemented as a convenience wrapper over `sys_sigaction`:

- installs `handler`
- clears `sa_mask`
- sets `SA_RESTART`
- leaves `sa_restorer = NULL`

## `rt_sigaction`

MOS implements the Linux/i386 `rt_sigaction` userspace layout:

```c
struct rt_sigaction_user {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    unsigned long sa_mask[2];
};
```

Current behavior:

- `sigsetsize` must be exactly `8`
- only the low 32 bits of the mask are used
- writeback clears the high word to zero

That matches the glibc 2.3.x expectations this userspace depends on.

## `pause`, `rt_sigsuspend`, and `rt_sigtimedwait`

### `pause`

`sys_pause()` simply sleeps until some unmasked signal becomes deliverable, then returns `-EINTR`.

### `rt_sigsuspend`

`sys_rt_sigsuspend()`:

- saves the current mask
- installs a temporary mask with `SIGKILL` and `SIGSTOP` forcibly unblocked
- calls `ps_signal_wait()` to sleep atomically
- leaves the temporary mask active
- sets `saved_sigmask` and `restore_sigmask`
- returns `-EINTR`

The old mask is not restored inside `rt_sigsuspend()` itself. It is restored later by `do_signal()` after the pending signal is handled or ignored. That preserves Linux-style semantics.

### `rt_sigtimedwait`

`sys_rt_sigtimedwait()`:

- waits for any signal in the supplied set to become pending
- consumes and returns the lowest-numbered matching signal
- returns `-EAGAIN` on timeout
- returns `-EINTR` if a non-waited unblocked signal becomes deliverable

Current limitation:

- `info` is only zero-filled; MOS does not build a rich `siginfo` result

## `sigaltstack`

`sys_sigaltstack()` supports install, query, and disable.

Current rules:

- changing the altstack while already executing on it returns `-EPERM`
- `ss_size < MINSIGSTKSZ` returns `-ENOMEM`
- `SS_DISABLE` clears the installed altstack

## Timers and `SIGALRM`

The signal subsystem shares one real-time alarm state per task:

- `alarm_expire_ms`
- `alarm_interval_ms`

### `alarm(2)`

`sys_alarm(seconds)`:

- returns the remaining alarm time in seconds
- arms or cancels the one-shot timer
- clears any interval behavior

### `setitimer(ITIMER_REAL)` / `getitimer(ITIMER_REAL)`

Current support is only for `which == 0` (`ITIMER_REAL`).

`sys_setitimer()`:

- exports the old timer state if requested
- sets one-shot and interval state in milliseconds
- uses the same `alarm_expire_ms` / `alarm_interval_ms` fields as `alarm(2)`

`check_alarm()` in `do_signal()`:

- raises `SIGALRM` when expiry is reached
- re-arms the timer if `alarm_interval_ms != 0`

## Pending-Set Queries

`sys_rt_sigpending()` returns:

- `sig_pending & sig_mask`

That means only signals that are both pending and currently blocked are reported.

## Scheduling Interaction

Two scheduler helpers matter for signal correctness:

- `time_wait(ms)` puts the task to sleep and optionally arms a wakeup timer
- `ps_signal_wait()` checks `sig_pending & ~sig_mask` under `ps_lock` before sleeping

That closes the classic lost-wakeup race between "check for signal" and "go to sleep".

## `fork()` and `execve()` Semantics

### `fork`

`fork_dup_signal()` copies the parent's signal context, then clears the child's pending set:

- handlers are inherited
- masks are inherited
- pending signals are not inherited

The child's real-time alarm state is reset separately in the fork path, so an already-armed parent alarm does not accidentally fire in the child.

### `execve`

On exec:

- caught handlers are reset to default
- ignored handlers stay ignored
- pending signals are cleared
- `saved_sigmask` / `restore_sigmask` are cleared
- the altstack is disabled

That matches the expected Unix reset behavior closely enough for the current userspace.

## Limitations

- Standard signals are bitmask-based, so repeated identical signals coalesce.
- `rt_sigqueueinfo()` does not queue full payloads.
- `rt_sigtimedwait()` does not return a rich `siginfo`.
- Only the low 32 signal bits are meaningful.
- There is no full Linux stop/continue job-control state machine here; the implementation is focused on the semantics needed by this userspace.
