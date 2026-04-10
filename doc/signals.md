# Signal Handling

**Source:** `src/ps/ps_signal.c`
**Headers:** `include/ps/signal.h`

---

## Overview

| Component              | File           | Responsibility                                                                                  |
| ---------------------- | -------------- | ----------------------------------------------------------------------------------------------- |
| Signal data structures | `signal.h`     | `sigaction`, `signal_frame`, `rt_signal_frame`, signal numbers, masks, altstack                 |
| Delivery engine        | `ps_signal.c`  | `do_signal` — builds frame, redirects `iret`                                                    |
| Return trampoline      | `signal_frame` | Inline 8-byte `__NR_sigreturn` / `__NR_rt_sigreturn` stub embedded in the signal frame on stack |
| Syscall interface      | `ps_signal.c`  | `sys_kill`, `sys_sigaction`, `sys_rt_sigaction`, `sys_sigprocmask`, `sys_rt_sigprocmask`,       |
|                        |                | `sys_sigreturn`, `sys_rt_sigreturn`, `sys_alarm`, `sys_pause`, `sys_sigaltstack`,               |
|                        |                | `sys_rt_sigpending`, `sys_rt_sigtimedwait`, `sys_rt_sigqueueinfo`, `sys_rt_sigsuspend`          |

---

## 1. Per-task signal state

Each task has a `signal_context *` in its `task_struct`:

```c
typedef struct _signal_context {
    struct sigaction sig_handlers[NSIG]; // handler for each signal 1–31
    sigset_t         sig_pending;        // bitmask: bit (N-1) = signal N pending
    sigset_t         sig_mask;           // bitmask: blocked signals
    sigset_t         saved_sigmask;      // mask saved by sigsuspend/pselect
    int              restore_sigmask;    // 1 → restore saved_sigmask after delivery
    stack_t          altstack;           // alternate signal stack (sigaltstack)
} signal_context;
```

`sigset_t` is `unsigned long` (32 bits). Bit position `sig - 1` corresponds to signal number `sig`.

```c
struct sigaction {
    void    (*sa_handler)(int);   // SIG_DFL / SIG_IGN / user function
    sigset_t  sa_mask;            // additional signals to block during handler
    int       sa_flags;           // SA_NODEFER | SA_RESETHAND | SA_SIGINFO | SA_ONSTACK | …
    void    (*sa_restorer)(void); // used when SA_RESTORER is set in sa_flags
};
```

---

## 2. Signal numbers

Linux/i386 ABI (1–31, `NSIG = 32`):

| Signal     | Number | Default action                               |
| ---------- | ------ | -------------------------------------------- |
| `SIGHUP`   | 1      | terminate                                    |
| `SIGINT`   | 2      | terminate                                    |
| `SIGQUIT`  | 3      | terminate                                    |
| `SIGILL`   | 4      | terminate                                    |
| `SIGKILL`  | 9      | terminate (cannot be caught/ignored/blocked) |
| `SIGSEGV`  | 11     | terminate                                    |
| `SIGALRM`  | 14     | terminate                                    |
| `SIGTERM`  | 15     | terminate                                    |
| `SIGCHLD`  | 17     | **ignore**                                   |
| `SIGCONT`  | 18     | **ignore**                                   |
| `SIGSTOP`  | 19     | stop (cannot be caught/ignored/blocked)      |
| `SIGWINCH` | 28     | **ignore**                                   |
| `SIGURG`   | 23     | **ignore**                                   |
| all others | —      | terminate                                    |

`SIGKILL` and `SIGSTOP` always call `sys_exit(sig | 0x80)` — they can never be caught, ignored, or blocked.

---

## 3. Return trampoline — inline in signal frame

When a signal handler returns (`ret`), execution must jump to the kernel's `sys_sigreturn` so the interrupted context can be restored.

**MOS does not use a vDSO for this.** Instead, the kernel writes a small 8-byte machine code stub directly into the signal frame on the user stack (`signal_frame.trampoline[]`):

```
b8 77 00 00 00    mov $119, %eax    # __NR_sigreturn
cd 80             int $0x80         # syscall
90                nop               # padding
```

`signal_frame.return_addr` points to `&sf->trampoline[0]`.  When the handler executes `ret`, it pops `return_addr` and jumps to the inline trampoline, which calls `sys_sigreturn` (119) or `sys_rt_sigreturn` (173) depending on the frame type.

---

## 4. Signal delivery: `do_signal`

```c
void do_signal(intr_frame *frame);
```

Called just before `iret` returns to user space:
- **End of every syscall** — `syscall_process()` in `syscall.c`
- **Timer interrupt** — PIT ISR in `time.c`

Guard: only acts when `frame->cs == USER_CODE_SELECTOR` (returning to ring 3).

### Delivery sequence

```
1. Check alarm:
     if alarm_expire_ms > 0 && time_now_ms() >= alarm_expire_ms:
         if alarm_interval_ms > 0:
             alarm_expire_ms = now + alarm_interval_ms    ← repeating alarm
         else:
             alarm_expire_ms = 0                          ← one-shot alarm
         sig_pending |= SIGALRM bit

2. deliverable = sig_pending & ~sig_mask
   if deliverable == 0: return (nothing to do)

3. Pick lowest-numbered deliverable signal (bit scan from sig 1 upward)

4. Clear the bit in sig_pending

5. Dispatch by handler:

   SIGKILL / SIGSTOP:
     sys_exit(sig | 0x80)             ← unconditional; cannot be overridden

   SIG_IGN:
     maybe_restore_sigmask()           ← restore sigsuspend mask if active
     return

   SIG_DFL:
     SIGCHLD / SIGURG / SIGWINCH / SIGCONT:
         maybe_restore_sigmask(); return  ← default = ignore
     all others:
         do_exit(sig)                  ← terminate process

   user handler:
     is_rt = (sa->sa_flags & SA_SIGINFO)
     resolve_sigstack()               ← altstack or current ESP
     if is_rt: build_rt_frame()       ← SA_SIGINFO path
     else:      build_legacy_frame()  ← legacy path
     update sig_mask, SA_RESETHAND
     frame->eip = handler; frame->esp = new_esp
```

---

## 5. Legacy signal frame (`signal_frame`)

Used when `SA_SIGINFO` is **not** set.

```c
typedef struct _signal_frame {
    unsigned int return_addr;    // → &trampoline[0] in this frame
    int          signo;          // first argument to handler(int signo)
    unsigned int saved_eip;
    unsigned int saved_eflags;
    unsigned int saved_esp;
    unsigned int saved_eax;
    unsigned int saved_ebx;
    unsigned int saved_ecx;
    unsigned int saved_edx;
    unsigned int saved_esi;
    unsigned int saved_edi;
    unsigned int saved_ebp;
    unsigned int saved_ds;       // segment registers saved/restored
    unsigned int saved_es;
    unsigned int saved_fs;
    unsigned int saved_gs;
    unsigned long saved_mask;    // signal mask before delivery
    unsigned char trampoline[8]; // inline __NR_sigreturn stub
} signal_frame;
```

**Stack view when handler starts executing** (ESP → `return_addr`):

```
[ESP+ 0]  return_addr   → &trampoline[0]  ← "ret" jumps here
[ESP+ 4]  signo         ← handler's argument
[ESP+ 8]  saved_eip
[ESP+12]  saved_eflags
[ESP+16]  saved_esp
[ESP+20]  saved_eax
  …         (saved registers + segment registers)
[ESP+64]  saved_mask
[ESP+68]  trampoline[8] ← "mov $119,%eax; int $0x80; nop"
```

`saved_mask` is the pre-delivery `sig_mask` (or the pre-sigsuspend `saved_sigmask` if `restore_sigmask` is set).

---

## 6. RT signal frame (`rt_signal_frame`)

Used when `SA_SIGINFO` is set. This is a larger frame compatible with the Linux/i386 `ucontext_t` layout expected by glibc.

```c
typedef struct _rt_signal_frame {
    unsigned int     pretcode;        // → sa_restorer or &retcode[0]
    int              sig;             // signal number
    unsigned int     pinfo;           // → &info
    unsigned int     puc;             // → &uc
    rt_siginfo_user  info;            // 128 bytes: si_signo, si_errno, si_code
    rt_ucontext_user uc;              // ucontext: flags, link, altstack, mcontext, sigmask
    rt_fpstate_user  fpstate;         // FPU state (zeroed; no FP save)
    unsigned char    retcode[8];      // inline __NR_rt_sigreturn stub (fallback)
} rt_signal_frame;
```

`uc.uc_mcontext` (`rt_sigcontext_user`) holds all GP registers, segment registers, `eip`, `eflags`, and `esp_at_signal`.

`pretcode` is set to `sa->sa_restorer` when `SA_RESTORER` is set; otherwise the inline `retcode[]` stub (`mov $173,%eax; int $0x80`) is used.

---

## 7. Signal return

### `sys_sigreturn` (syscall 119) — legacy

Triggered by the inline trampoline after the handler's `ret`. At that point:
- `user ESP = signal_frame_base + 4` (return_addr has been consumed by `ret`)
- `sf = (signal_frame *)(frame->esp - 4)`

Restores all GP registers, segment registers, `eip`, `eflags`, `esp`, and `sig_mask` from the frame. Returns `sf->saved_eax` so the interrupted `eax` is preserved.

If the task was executing on the altstack and the restored `esp` falls outside the altstack range, `SS_ONSTACK` is cleared.

### `sys_rt_sigreturn` (syscall 173) — SA_SIGINFO

Same principle. Restores from `rt_signal_frame.uc.uc_mcontext` and `uc.uc_sigmask`. Handles altstack cleanup the same way.

---

## 8. Alternate signal stack (`sigaltstack`)

`sys_sigaltstack` (syscall 186) installs or queries the per-task alternate stack.

```c
int sys_sigaltstack(const stack_t *ss, stack_t *old_ss);
```

- Cannot be changed while the task is already executing on the altstack (`SS_ONSTACK`).
- `ss->ss_size` must be ≥ `MINSIGSTKSZ` (2048) unless disabling.
- `SS_DISABLE` clears the altstack.

`do_signal` calls `resolve_sigstack()`:
- If `SA_ONSTACK` is set, the altstack is installed and not disabled/active → use altstack top, set `SS_ONSTACK`.
- Otherwise use current `frame->esp`.

---

## 9. Sending signals

### `ps_send_signal(pid, sig)`

```c
target->signal->sig_pending |= (1UL << (sig - 1));
if (target->status == ps_waiting &&
    !(target->signal->sig_mask & (1UL << (sig - 1))))
    ps_put_to_ready_queue(target);   // wake only if signal is unmasked
```

Only wakes the target if the arriving signal is not masked. This prevents spurious wakeups from `sigsuspend`.

Permission check via `can_send_signal(sender, target)`: sender's `euid == 0`, or uid/euid matches target's uid/suid.

### `sys_kill(pid, sig)`

| `pid`      | Behaviour                                        |
| ---------- | ------------------------------------------------ |
| `> 0`      | Send to process `pid`                            |
| `== 0`     | Send to all processes in current process group   |
| `== -1`    | Send to all other user processes (broadcast)     |
| `< -1`     | Send to process group `abs(pid)`                 |
| `sig == 0` | Existence/permission check only (no signal sent) |

---

## 10. Signal-related syscalls

| Syscall               | Number | Description                                       |
| --------------------- | ------ | ------------------------------------------------- |
| `sys_kill`            | 37     | Send signal to process / process group            |
| `sys_alarm`           | 27     | Set `SIGALRM` timer (seconds)                     |
| `sys_pause`           | 29     | Block until any signal is deliverable             |
| `sys_sigaction`       | 67     | Get/set `sigaction` (legacy)                      |
| `sys_sigreturn`       | 119    | Restore context after legacy handler              |
| `sys_sigprocmask`     | 126    | Block/unblock/replace signal mask (legacy)        |
| `sys_rt_sigaction`    | 174    | Get/set `sigaction` (glibc 2.3.2 RT layout)       |
| `sys_rt_sigprocmask`  | 175    | Block/unblock/replace signal mask (8-byte sigset) |
| `sys_rt_sigpending`   | 176    | Return set of pending blocked signals             |
| `sys_rt_sigtimedwait` | 177    | Wait for signal from a set (with timeout)         |
| `sys_rt_sigqueueinfo` | 178    | Send signal + siginfo to process                  |
| `sys_rt_sigsuspend`   | 179    | Atomically set mask and suspend                   |
| `sys_rt_sigreturn`    | 173    | Restore context after SA_SIGINFO handler          |
| `sys_sigaltstack`     | 186    | Get/set alternate signal stack                    |

### `sys_rt_sigaction`

glibc's `sigaction(3)` on i386 uses `rt_sigaction` with a different userspace layout:

```c
struct rt_sigaction_user {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    unsigned long sa_mask[32];  // 128-byte sigset
};
```

Only `sa_mask[0]` (low 32 bits) is used internally; upper words are zeroed on writeback.

### `sys_rt_sigprocmask`

Like `sys_sigprocmask` but reads/writes 8-byte sigsets (upper 4 bytes are always 0). `SIGKILL` and `SIGSTOP` bits are always cleared after any `sigprocmask`.

### `sys_alarm`

```c
unsigned sys_alarm(unsigned seconds);
```

Sets `task->alarm_expire_ms = now_ms() + seconds*1000`. Returns remaining time.  
`seconds == 0` cancels the alarm.  
`do_signal` fires `SIGALRM` and re-arms if `alarm_interval_ms > 0` (repeating alarm).

### `sys_rt_sigpending`

Returns `sig_pending & sig_mask` — signals that are both pending and blocked (not yet delivered because masked).

### `sys_rt_sigtimedwait`

Waits for any signal in `set` to become pending. The signals in `set` should already be blocked. Returns the signal number on success, `-EAGAIN` on timeout, `-EINTR` if an unblocked, non-waited signal arrives.

### `sys_rt_sigsuspend`

Atomically replaces `sig_mask` with `*mask`, calls `ps_signal_wait()` to sleep until an unmasked signal arrives, then sets `restore_sigmask = 1` (so `do_signal` restores `saved_sigmask` after handler). Always returns `-EINTR`.

The mask is **not** restored here; `do_signal` handles the restore so that the signal is delivered under the correct (temporary sigsuspend) mask before the original mask is put back.

---

## 11. End-to-end lifecycle

```
User process running at ring 3
  │
  │  Ctrl-C pressed  → TTY ldisc → ps_send_signal_pgrp(pgrp, SIGINT)
  │                              → target->sig_pending |= SIGINT bit
  │                              → wake target if sleeping and SIGINT unmasked
  │
  ↓  (next syscall or timer interrupt returns to user mode)

do_signal(frame):
  deliverable = sig_pending & ~sig_mask   ← SIGINT is deliverable
  sig = 2  (SIGINT)
  sig_pending &= ~SIGINT bit
  is_rt = (sa->sa_flags & SA_SIGINFO)     ← choose frame type
  new_esp = frame->esp - sizeof(frame), 16-byte aligned
  build_legacy_frame() or build_rt_frame()
    sf->return_addr = &sf->trampoline[0]  ← inline stub in frame
    sf->saved_*     = frame->*            ← all registers
    sf->saved_mask  = sig_mask
    sf->trampoline  = "mov $119,%eax; int $0x80; nop"
  sig_mask |= SIGINT bit                  ← block SIGINT during handler
  frame->eip = user_ctrl_c_handler
  frame->esp = new_esp

iret → user_ctrl_c_handler(2) runs at ring 3
  handler body executes
  ret  →  pops return_addr  →  jumps to sf->trampoline[]

trampoline (on user stack):
  mov $119, %eax
  int $0x80                 ← sys_sigreturn

sys_sigreturn:
  sf = (signal_frame *)(frame->esp - 4)
  frame->eip    = sf->saved_eip
  frame->esp    = sf->saved_esp
  frame->eax    = sf->saved_eax
  … all registers restored …
  sig_mask      = sf->saved_mask   ← SIGINT unblocked again

do_signal(frame):   ← called again after sys_sigreturn
  no more pending signals → return

iret → original user code resumes exactly where it was interrupted
```
