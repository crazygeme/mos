# Signal Handling

**Source:** `src/syscall/syscall_proc.c`, `src/mm/vdso.c`
**Headers:** `include/ps/signal.h`, `include/mm/vdso.h`

---

## Overview

| Component | File | Responsibility |
|-----------|------|---------------|
| Signal data structures | `signal.h` | `sigaction`, `signal_frame`, signal numbers, masks |
| Delivery engine | `syscall_proc.c` | `do_signal` — builds frame, redirects `iret` |
| Return trampoline | `vdso.c` | `vdso_sigreturn_tramp` — user-space `sys_sigreturn` stub |
| vDSO mapping | `vdso.c` | `mm_vdso_map` — maps kernel trampoline code into user space |
| Syscall interface | `syscall_proc.c` | `sys_kill`, `sys_sigaction`, `sys_sigprocmask`, `sys_sigreturn`, `sys_alarm`, `sys_pause` |

---

## 1. Per-task signal state

Each task has a `signal_context *` in its `task_struct`:

```c
typedef struct _signal_context {
    struct sigaction sig_handlers[NSIG];  // handler for each signal 1–31
    sigset_t         sig_pending;         // bitmask: bit (N-1) = signal N pending
    sigset_t         sig_mask;            // bitmask: blocked signals
} signal_context;
```

`sigset_t` is `unsigned long` (32 bits). Bit position `sig - 1` corresponds to signal number `sig`.

```c
struct sigaction {
    void    (*sa_handler)(int);   // SIG_DFL / SIG_IGN / user function
    sigset_t  sa_mask;            // additional signals to block during handler
    int       sa_flags;           // SA_NODEFER | SA_RESETHAND | SA_RESTART | …
    void    (*sa_restorer)(void); // (not used; trampoline is vDSO-provided)
};
```

---

## 2. Signal numbers

Linux/i386 ABI (1–31, `NSIG = 32`):

| Signal | Number | Default action |
|--------|--------|---------------|
| `SIGHUP` | 1 | terminate |
| `SIGINT` | 2 | terminate |
| `SIGILL` | 4 | terminate |
| `SIGKILL` | 9 | terminate (cannot be caught/ignored/blocked) |
| `SIGSEGV` | 11 | terminate |
| `SIGALRM` | 14 | terminate |
| `SIGTERM` | 15 | terminate |
| `SIGCHLD` | 17 | **ignore** |
| `SIGCONT` | 18 | **ignore** |
| `SIGSTOP` | 19 | stop (cannot be caught/ignored/blocked) |
| `SIGWINCH` | 28 | **ignore** |
| `SIGURG` | 23 | **ignore** |
| all others | — | terminate |

`SIGKILL` and `SIGSTOP` always call `sys_exit(sig | 0x80)` / stop — they can never be caught, ignored, or blocked.

---

## 3. vDSO — signal return trampoline

### Why vDSO is needed

When a signal handler returns (executes `ret`), it pops `return_addr` from the stack and jumps to it. The kernel needs to regain control at that point so it can call `sys_sigreturn` to restore the interrupted context. The trampoline code that does this must be executable in user space.

Rather than copying machine code bytes onto the stack (the old Linux approach), MOS maps a dedicated read-only kernel code region into every user process at a fixed virtual address (`VDSO_MM_REGION = 0x10000`). This region, the **vDSO**, contains the trampoline function.

### vDSO section

```c
#define _VDSO __attribute__((used, section(".vdso")))
#define VDSO_MM_REGION 0x10000
```

All functions tagged `_VDSO` are placed into the `.vdso` linker section. The linker script exposes `__vdso_start` and `__vdso_end` so the kernel knows the physical extent of the section.

### Trampoline: `vdso_sigreturn_tramp`

```c
_VDSO NAKED void vdso_sigreturn_tramp()
{
    asm volatile("mov $119, %eax");  // __NR_sigreturn = 119
    asm volatile("int $0x80");       // syscall → sys_sigreturn
    NOP();                           // padding
}
```

This is the only code the handler needs to return through. It invokes `sys_sigreturn` (syscall 119) which restores the interrupted user context.

### Mapping: `mm_vdso_map`

Called by `sys_execve` after loading the ELF image:

```c
void mm_vdso_map()
{
    // compute physical page range of .vdso section
    // map each page at VDSO_MM_REGION + i*PAGE_SIZE
    // flags: PAGE_ENTRY_USER_CODE (present, user, read-only, not writable)
    RELOAD_CR3();
}
```

The vDSO pages are the **same physical pages** as the kernel's `.vdso` section — they are not copied. Every process shares the same physical pages, mapped read-only and execute-only at `0x10000`.

### Address translation: `mm_vdso_translate`

```c
unsigned mm_vdso_translate(unsigned kernel_code)
{
    return VDSO_MM_REGION + (kernel_code - (unsigned)&__vdso_start);
}
```

During signal delivery, `do_signal` uses this to find the user-space address of `vdso_sigreturn_tramp`:

```c
sf->return_addr = (unsigned int)mm_vdso_translate(vdso_sigreturn_tramp);
```

---

## 4. Signal delivery: `do_signal`

```c
void do_signal(intr_frame *frame);
```

Called at two points, always just before `iret` returns to user space:
- **End of every syscall** — `syscall_process()` in `syscall.c`
- **Timer interrupt** — PIT ISR in `time.c` (ensures signals fire even in tight user loops)
- **`ret_from_fork`** — newly-forked children call it directly from assembly before their first `iret`

Guard: only acts when `frame->cs == USER_CODE_SELECTOR` (returning to ring 3).

### Delivery sequence

```
1. Check alarm:
     if alarm_expire_ms > 0 && time_now_ms() >= alarm_expire_ms:
         alarm_expire_ms = 0
         sig_pending |= SIGALRM bit

2. deliverable = sig_pending & ~sig_mask
   if deliverable == 0: return (nothing to do)

3. Pick lowest-numbered deliverable signal (bit scan from sig 1 upward)

4. Clear the bit in sig_pending

5. Dispatch by handler:

   SIGKILL / SIGSTOP:
     sys_exit(sig | 0x80)        ← unconditional; cannot be overridden

   SIG_IGN:
     return                      ← discard silently

   SIG_DFL:
     SIGCHLD / SIGURG / SIGWINCH / SIGCONT → return (default = ignore)
     all others → do_exit(sig)   ← terminate process

   user handler:
     build signal_frame on user stack  (see §5)
     redirect iret → handler
```

---

## 5. Signal frame

When a user-defined handler is to be called, `do_signal` allocates a `signal_frame` on the **user stack** (below the current `frame->esp`, then 16-byte aligned):

```c
typedef struct _signal_frame {
    unsigned int return_addr;    // → vdso_sigreturn_tramp (user VA)
    int          signo;          // first argument to handler(int signo)
    unsigned int saved_eip;      // interrupted instruction pointer
    unsigned int saved_eflags;
    unsigned int saved_esp;      // stack pointer at time of interrupt
    unsigned int saved_eax;      // all GP registers
    unsigned int saved_ebx;
    unsigned int saved_ecx;
    unsigned int saved_edx;
    unsigned int saved_esi;
    unsigned int saved_edi;
    unsigned int saved_ebp;
    unsigned long saved_mask;    // signal mask before delivery
} signal_frame;
```

**Stack view when handler starts executing** (ESP → `return_addr`):

```
[ESP+ 0]  return_addr   → vdso_sigreturn_tramp  ← "ret" jumps here
[ESP+ 4]  signo         ← handler's argument
[ESP+ 8]  saved_eip
[ESP+12]  saved_eflags
[ESP+16]  saved_esp
[ESP+20]  saved_eax
  …         (saved registers)
[ESP+44]  saved_ebp
[ESP+48]  saved_mask
```

After the frame is built:

```c
// Block this signal during handler (unless SA_NODEFER)
if (!(sa->sa_flags & SA_NODEFER))
    cur->signal->sig_mask |= (1UL << (sig - 1));
cur->signal->sig_mask |= sa->sa_mask;  // plus sa->sa_mask

// SA_RESETHAND: revert disposition to SIG_DFL after first delivery
if (sa->sa_flags & SA_RESETHAND)
    sa->sa_handler = SIG_DFL;

// Redirect iret to the handler
frame->eip = handler;
frame->esp = new_esp;
```

The next `iret` instruction in the interrupt return path will land directly in the user-space handler with ESP pointing at the signal frame.

---

## 6. Signal return: `sys_sigreturn`

**Syscall 119** (`__NR_sigreturn`). Triggered by the vDSO trampoline after the handler executes `ret`.

When `ret` executes:
- It pops `return_addr` from `[ESP]` and jumps to `vdso_sigreturn_tramp`.
- At this point `ESP = signal_frame_base + 4` (return_addr has been consumed).

`sys_sigreturn` locates the frame:

```c
intr_frame   *frame = top of current task's kernel stack
signal_frame *sf    = (signal_frame *)(frame->esp - 4)
                    //  frame->esp == signal_frame_base + 4
                    //  sf         == signal_frame_base
```

It then restores all saved registers into the kernel interrupt frame:

```c
frame->eip    = sf->saved_eip;
frame->eflags = sf->saved_eflags;
frame->esp    = sf->saved_esp;
frame->eax    = sf->saved_eax;
// … all GP registers …
cur->signal->sig_mask = sf->saved_mask;   // restore pre-delivery mask
```

The syscall returns `sf->saved_eax` — so when `syscall_process` writes the return value into `frame->eax`, it writes back the original `eax` the user had before the signal interrupted them.

After `sys_sigreturn`, `do_signal` is called again (the normal post-syscall path): if another signal is pending it can be delivered immediately.

---

## 7. Sending signals

### `ps_send_signal(pid, sig)`

```c
target->signal->sig_pending |= (1UL << (sig - 1));
if (target->status == ps_waiting)
    ps_put_to_ready_queue(target);   // wake sleeping target
```

Sets the pending bit atomically and wakes the target if it is blocked in `ps_waiting`.

### `ps_send_signal_pgrp(pgrp, sig)`

Iterates the global `mgr_queue` RB-tree; calls `ps_send_signal` for every user task whose `group_id == pgrp`. Used by:
- **TTY line discipline** — sends `SIGINT` / `SIGQUIT` / `SIGTSTP` when Ctrl-C / Ctrl-\ / Ctrl-Z is typed.
- **TTY write** — sends `SIGTTOU` to background process groups trying to write to the terminal.

### `sys_kill(pid, sig)`

Thin wrapper: `sig == 0` → existence check only; otherwise calls `ps_send_signal(pid, sig)`.

---

## 8. Signal-related syscalls

| Syscall | Number | Description |
|---------|--------|-------------|
| `sys_kill` | 37 | Send signal to process |
| `sys_sigaction` | 67 | Get/set `sigaction` for a signal |
| `sys_sigprocmask` | 126 | Block/unblock/replace signal mask |
| `sys_sigreturn` | 119 | Restore context after handler |
| `sys_alarm` | 27 | Set `SIGALRM` timer (seconds) |
| `sys_pause` | 29 | Block until any signal is deliverable |
| `sys_sigaction` (rt) | 174 | `rt_sigaction` alias |
| `sys_sigprocmask` (rt) | 175 | `rt_sigprocmask` alias |

### `sys_sigaction`

```c
int sys_sigaction(int sig, struct sigaction *act, struct sigaction *oact);
```

- `SIGKILL` and `SIGSTOP` return `-EINVAL` (cannot be overridden).
- If `oact != NULL`: copy old handler out.
- If `act != NULL`: install new handler.

### `sys_sigprocmask`

```c
int sys_sigprocmask(int how, sigset_t *set, sigset_t *oset);
```

`how` values: `SIG_BLOCK` (OR), `SIG_UNBLOCK` (AND NOT), `SIG_SETMASK` (replace).
`SIGKILL` and `SIGSTOP` bits are **always cleared** from the mask after any `sigprocmask` call — they can never be blocked.

### `sys_alarm`

```c
unsigned sys_alarm(unsigned seconds);
```

Sets `task->alarm_expire_ms = now_ms() + seconds*1000`.
Returns remaining time of any previously set alarm.
`seconds == 0` cancels the alarm.
`do_signal` fires `SIGALRM` when `time_now_ms() >= alarm_expire_ms`.

### `sys_pause`

Spins with `task_sched()` until `sig_pending & ~sig_mask != 0`, then returns `-EINTR`.

---

## 9. End-to-end lifecycle

```
User process running at ring 3
  │
  │  Ctrl-C pressed  → TTY ldisc → ps_send_signal_pgrp(pgrp, SIGINT)
  │                              → target->sig_pending |= SIGINT bit
  │                              → wake target if sleeping
  │
  ↓  (next syscall or timer interrupt returns to user mode)

do_signal(frame):
  deliverable = sig_pending & ~sig_mask   ← SIGINT is deliverable
  sig = 2  (SIGINT)
  sig_pending &= ~SIGINT bit
  sa->sa_handler = user_ctrl_c_handler
  new_esp = frame->esp - sizeof(signal_frame), 16-byte aligned
  sf->return_addr = mm_vdso_translate(vdso_sigreturn_tramp)  ← 0x10000+offset
  sf->signo       = 2
  sf->saved_eip   = frame->eip     ← interrupted instruction
  sf->saved_*     = frame->*       ← all registers
  sf->saved_mask  = sig_mask
  sig_mask |= SIGINT bit           ← block SIGINT during handler
  frame->eip = user_ctrl_c_handler
  frame->esp = new_esp

iret → user_ctrl_c_handler(2) runs at ring 3
  handler body executes
  ret  →  pops return_addr  →  jumps to 0x10000+offset (vdso_sigreturn_tramp)

vdso_sigreturn_tramp:
  mov $119, %eax
  int $0x80                 ← sys_sigreturn

sys_sigreturn:
  sf = (signal_frame *)(frame->esp - 4)
  frame->eip    = sf->saved_eip    ← original interrupted instruction
  frame->esp    = sf->saved_esp
  frame->eax    = sf->saved_eax
  … all registers restored …
  sig_mask      = sf->saved_mask   ← SIGINT unblocked again
  return sf->saved_eax

do_signal(frame):   ← called again after sys_sigreturn
  no more pending signals → return

iret → original user code resumes exactly where it was interrupted
```
