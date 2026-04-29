# Bug Fix Journal

A running log of non-obvious bugs, their symptoms, root causes, and fixes.
Each entry explains the reasoning so the same mistake is not repeated.

---

## 2026-04-29 - RH9 `LABEL=/` fstab root remount failed before `mount(2)`

Real Red Hat 9 systems commonly use an `/etc/fstab` root entry whose source is
`LABEL=/`:

```text
LABEL=/  /  ext3  defaults  1 1
```

MOS could boot the same image only after changing that source to `/`, but that
replacement was not compatible with the real Linux boot path.

### 1. `mount` failed while resolving the label in userspace

- **Caught by:** tracing the boot log around the
  `Remounting root filesystem in read-write mode` step and comparing it with
  the Red Hat 9 util-linux 2.11y `mount` source.
- **Symptom:** `/bin/mount` printed `mount: no such partition found` and
  exited with status `100` during `mount -n -o remount,rw /`. No kernel
  `mount(2)` syscall for the root remount appeared after the failure.
- **Key log evidence:**
  - `/bin/mount` read the root fstab entry as `LABEL=/ ... / ... ext3 ...`
  - it opened `/proc/partitions` and found `hda1`
  - it opened `/dev/hda1` successfully
  - both block-device size ioctls failed:

```text
ioctl(4, 80041272, ...) = -25
ioctl(4, 1260, ...) = -25
```

- **Root cause:** RH9 `mount` resolves `LABEL=/` before calling `mount(2)`.
  Its label probe scans `/proc/partitions`, opens each candidate block device,
  asks block-device size ioctls such as `BLKGETSIZE64` and `BLKGETSIZE`, then
  reads the ext2/ext3 superblock at offset 1024 to compare the volume label.
  MOS exposed `/proc/partitions` and `/dev/hda1`, but the HDD block device did
  not implement those ioctls, so userspace stopped before reading the
  superblock and never issued the remount syscall.
- **Fix:**
  - in [ioctl.h](../include/fs/ioctl.h), define Linux-compatible
    `BLKGETSIZE`, `BLKSSZGET`, and `BLKGETSIZE64`
  - in [hdd.c](../src/dev/hdd.c), implement those ioctls for IDE partition
    block devices using the discovered partition sector count
  - in [loop.c](../src/dev/loop.c), implement the same block-size ioctls for
    configured loop devices so label and filesystem probes see normal block
    device behavior there as well

### Key lesson

Boot compatibility with old Linux userspace can depend on pre-syscall probing
behavior. A failing `mount` command does not necessarily mean the kernel
rejected `mount(2)`; it may mean userspace could not resolve an fstab source
because a block-device compatibility ioctl was missing.

---

## 2026-04-29 - `exit_group()` could leave stale futex waiters on killed thread stacks

The observed crash was a kernel page fault in the release build:
`segfault: error code 0, address eedad000, eip c022f039`. Resolving the
release address placed the fault at `ps_futex_wake_locked+0x39`, while walking
the global futex waiter list.

### 1. Futex wake dereferenced a stale waiter node

- **Caught by:** resolving `c022f039` against the release kernel symbols and
  checking the generated instruction stream, which showed the faulting
  instruction loading the waiter state from the list entry being walked by
  `ps_futex_wake_locked()`.
- **Symptom:** the kernel faulted at `ps_futex_wake_locked()` while reading
  through an address near `0xeedad000`, below the kmap window and not in a
  normal user mapping.
- **Root cause:** `sys_futex(FUTEX_WAIT)` stores a `futex_waiter` object on the
  sleeping task's kernel stack and links that stack object into the global
  `futex_waiters` list. A normal futex wake or timeout removes the node before
  the waiter returns. However, `exit_group()` kills sibling threads by removing
  them from scheduler structures and then reaping their task pages. If one of
  those sibling threads was blocked in `FUTEX_WAIT`, its stack-resident waiter
  node could remain in the global futex list after the task page was freed.
  A later `FUTEX_WAKE` then walked the stale list entry and dereferenced freed
  stack memory.
- **Fix:**
  - in [syscall_futex.c](../src/syscall/syscall_futex.c), add
    `ps_futex_remove_task_locked()` to unlink any futex waiter owned by a task
    while `ps_lock` is held
  - in [ps_internal.h](../src/ps/ps_internal.h), expose the helper to process
    teardown code
  - in [ps_syscall.c](../src/ps/ps_syscall.c), call the helper from
    `ps_kill_thread_group()` before removing a killed sibling thread from the
    process manager and before its kernel stack can be reaped

### Key lesson

Any wait object stored on a task's kernel stack must be unlinked from global
wait queues before asynchronous thread teardown can free that stack. Scheduler
queue removal and timer disarming are not enough when a blocking syscall also
publishes a stack-local waiter through a subsystem-specific list.

---

## 2026-04-29 - `gnome-terminal` `Ctrl-C` still could not interrupt `ping`

This failure looked at first like the earlier raw-read wakeup bug, but the
remaining symptom was narrower: interactive programs that do not read stdin
still ignored `Ctrl-C` while running behind a PTY. The right approach was to
follow where MOS actually interprets PTY input bytes instead of assuming the
existing `ISIG` checks were enough.

### 1. PTY `Ctrl-C` depended on the foreground program reading stdin

- **Caught by:** tracing the PTY line-discipline path and comparing it with
  the already-working virtual-console path in
  [tty.c](../src/dev/tty.c).
- **Symptom:** pressing `Ctrl-C` in `gnome-terminal` still failed to stop
  `ping` immediately even though `Ctrl-C` worked better for programs blocked in
  terminal reads.
- **Root cause:** MOS handled PTY signal characters too late. On the PTY path,
  `VINTR`, `VQUIT`, and `VSUSP` were recognized only when the slave side later
  executed `read()`. That works for shells or utilities actively reading the
  terminal, but a foreground program like `ping` usually does not read stdin at
  all. The `^C` byte therefore remained queued in the PTY input buffer and no
  `SIGINT` reached the foreground process group.
- **Fix:**
  - in [pts.c](../src/dev/pts.c), move PTY signal-character handling to
    `pts_master_write()` so the line discipline interprets `VINTR`, `VQUIT`,
    and `VSUSP` at input-arrival time, matching real terminal behavior more
    closely
  - consume those signal characters before they enter the PTY slave input
    queue, so a foreground job that never reads stdin still receives the
    signal
  - clear the partial canonical buffer when such a signal character arrives,
    preserving the same line-reset behavior already used by canonical reads
  - remove the duplicate PTY read-side signal-generation path so the same byte
    cannot raise the signal a second time later

### Key lesson

For PTYs, `ISIG` handling must happen when input is delivered to the terminal,
not only when a process eventually reads from it. Otherwise foreground jobs
that do not consume stdin, such as `ping`, can never see `Ctrl-C` even though
the signal-character logic exists in the read path.

## 2026-04-29 - `gnome-terminal` lost immediate `Ctrl-C` and turned Up-arrow into `8`

This bug looked at first like a terminal-emulator or readline problem because
the visible failures happened inside `gnome-terminal` and bash. The useful
approach was to separate the two symptoms and follow each one down to the
kernel boundary instead of patching PTY behavior blindly.

### 1. `Ctrl-C` generated `SIGINT` only after a follow-up key

- **Caught by:** reproducing the shell behavior in `gnome-terminal`, then
  comparing MOS raw TTY/PTTY reads with bash 2.05b's real tty setup in
  [rltty.c](bash-2.05b/lib/readline/rltty.c).
- **Symptom:** pressing `Ctrl-C` did not interrupt the foreground program
  immediately. No visible effect occurred until a subsequent keystroke such as
  Backspace or an arrow key arrived.
- **Root cause:** bash/readline puts the terminal into noncanonical mode but
  keeps `ISIG` enabled. MOS raw console and PTY-slave read paths were updated
  to recognize signal characters and send `SIGINT`, `SIGQUIT`, or `SIGTSTP`,
  but they still stayed inside the same blocking read after consuming the
  signal byte. That left the caller asleep until another input byte woke the
  read path.
- **Fix:**
  - in [tty_ldisc.c](../src/dev/tty_ldisc.c) and
    [tty_ldisc.h](../src/dev/tty_ldisc.h), add a shared helper that recognizes
    `VINTR`, `VQUIT`, and `VSUSP` and signals the foreground process group
  - in [tty.c](../src/dev/tty.c) and [pts.c](../src/dev/pts.c), return
    `-EINTR` immediately when a raw read consumes only a signal character, or
    return the already-collected byte count when the signal arrives after data
  - in canonical handling, stop treating signal characters like ordinary input
    bytes when `ISIG` is active

### 2. Up-arrow intermittently arrived as keypad `8`

- **Caught by:** reading
  [out/x86/release/krn.log](../out/x86/release/krn.log), then correlating the
  raw keyboard reads with the X server's output into the PTY.
- **Symptom:** Up-arrow in `gnome-terminal` sometimes produced the expected
  cursor sequence, but often inserted a literal `8`.
- **Key log evidence:**
  - the good case showed `read(5, "\xe0H", 64) = 2`, followed by X writing
    `"\x1b[A"` to the PTY
  - the bad case showed `read(5, "H", 64) = 1` and `read(5, "\xc8", 64) = 1`,
    followed by pid `1604` writing `"8"` to the PTY
- **Why that matters:** bare `0x48` / `0xc8` is keypad 8 press/release in the
  PC set-1 stream, while extended cursor-Up is `0xe0 0x48` / `0xe0 0xc8`.
  The PTY and shell were behaving correctly; X was faithfully turning the scan
  codes it received into either Up or keypad 8.
- **Root cause:** MOS's keyboard DSR assumed that an `0xe0` prefix and the
  following scan byte were both available immediately. When the controller
  delivered them as separate bytes or separate interrupts, the prefix could be
  dropped and the next byte was emitted alone as `0x48`/`0xc8`. That made X
  interpret the cursor key as keypad 8.
- **Fix:**
  - in [keyboard.c](../src/hw/keyboard.c), make the keyboard DSR drain all
    pending keyboard bytes from the i8042 output buffer instead of processing
    only one logical code per callback
  - preserve an `0xe0`/`0xe1` prefix across bytes and combine it with the next
    scan byte before raw, medium-raw, or translated handling
  - keep ignoring auxiliary-device bytes in the keyboard path so mouse traffic
    stays owned by the PS/2 mouse driver

### 3. Linux console keyboard compatibility also needed Linux-style keysyms

- **Caught by:** checking how XFree86 4.3.0 reads `KDGKBENT` and keyboard mode
  ioctls from a Linux console.
- **Symptom:** even before the prefix-loss bug was isolated, MOS was exporting
  keyboard symbols that did not match Linux `keyboard.h`, which made extended
  key interpretation fragile for X.
- **Root cause:** MOS's `KDGKBENT` encoding and default keymap entries did not
  match the Linux console values that XFree86 expects for keypad, cursor, and
  modifier symbols.
- **Fix:**
  - in [ioctl.h](../include/fs/ioctl.h), switch the keysym type/value encoding
    and exported constants to Linux-compatible values
  - in [keyboard.c](../src/hw/keyboard.c), populate the default keymap entries
    for keypad, cursor, navigation, and right-side modifier keycodes with the
    expected Linux symbols

### Key lesson

When an old X stack turns an arrow key into a printable digit, inspect the raw
keyboard bytes before blaming the PTY or shell. Here the visible `"8"` was a
faithful consequence of MOS occasionally dropping the `0xe0` extended-key
prefix, and the `Ctrl-C` delay was a separate `-EINTR` wakeup bug in raw tty
reads.


## 2026-04-26 - `strace ps aux` over SSH could #GP on `intr_exit: pop %gs`

Initial triage suggested a ptrace or TLS setup bug because the visible crash
was always a user task dying with `#GP(error_code = 0x30)` while traced
through SSH. The decisive clue came from logging both the task's saved TLS
state and the live hardware GDT entries at fault time.

### 1. Traced `/usr/sbin/sshd` and `/bin/ps` faulted in `intr_exit`

- **Caught by:** logging `#GP` state in [int.c](../src/int/int.c) and matching
  the faulting EIP against [assemble.s](../out/x86/debug/assemble.s), which
  showed the crash at `intr_exit: pop %gs`.
- **Symptom:** `strace ps aux` in an SSH session could kill either `sshd` or
  the traced `/bin/ps` with logs like `gs=0x33` or `gs=0x1b`, always faulting
  in the interrupt-exit path rather than in ordinary userspace code.
- **Root cause:** MOS stores Linux TLS slots 6..8 in the shared CPU GDT even
  though their contents are per-task. Some ptrace-stop and nested
  interrupt/syscall-return paths reached `intr_exit` without refreshing those
  live GDT entries for the current task first. The logs showed the mismatch
  directly: the task's `tls_desc[0]` for slot 6 was valid while live `gdt[6]`
  was zero, so `pop %gs` revalidated selector `0x33` against an empty
  descriptor and raised `#GP(error_code = 0x30)`. The later `/bin/ps` traces
  also exposed a second half of the problem: MOS left user `%fs/%gs` live in
  ring 0, making nested interrupt returns depend on user selectors while still
  executing kernel code.
- **Fix:**
  - in [int.S](../src/int/int.S), switch `%fs` and `%gs` to
    `KERNEL_DATA_SELECTOR` on interrupt and syscall entry, just like `%ds/%es`
  - in [int.c](../src/int/int.c), reload the current task's live TLS GDT slots
    and LDT on every interrupt/syscall exit before any saved user `%gs` is
    restored
  - in [ps.c](../src/ps/ps.c), centralize that live TLS/LDT reload logic in
    `ps_load_task_segments()` so both the scheduler and interrupt-exit path use
    the same code
  - keep the already-needed clone/TLS fixes in [ps_clone.c](../src/ps/ps_clone.c)
    and [ps_tls.c](../src/ps/ps_tls.c) so new threads inherit only the active
    TLS state and plain `set_thread_area()` does not silently rewrite the
    saved user `%gs`


## 2026-04-26 - GUI Emacs mapped a frame but stalled before usable content

This issue required three separate kernel fixes because the visible symptom
changed as each blocker was removed. The important part was not to patch
around the blank window directly, but to keep explaining why old RH9 Emacs
behaved differently from simpler X clients.

### 1. `gettimeofday()` could move backward and trap Emacs in `SIGALRM`

- **Caught by:** reading [out/x86/release/krn.log](../out/x86/release/krn.log)
  during the GUI Emacs investigation, then comparing the timer path with the
  real Emacs 21.2 sources under
  [`emacs-21.2-rh9-src`](../emacs-21.2-rh9-src).
- **Symptom:** GUI `emacs` did not appear at all, and the syscall log showed a
  hot loop of `sig_deliver(14)`, `setitimer()`, and `gettimeofday()` during X
  startup.
- **Root cause:** MOS could return a wall-clock sample that jumped forward by
  one PIT tick and then backward on the next call. Emacs 21.2 uses
  `ITIMER_REAL` and `SIGALRM` for atimers during GUI startup, so that
  non-monotonic clock made its timer machinery spin instead of progressing
  through the X event loop.
- **Fix:** in [time.c](../src/hw/time.c), keep the IRQ-pending compensation for
  the PIT race, but only add a missing tick when the latched counter proves a
  wrap happened inside the same `tickets` epoch. This stopped the false
  one-jiffy jumps caused by noisy PIC IRR reads.

### 2. `ITIMER_REAL` was too fine-grained compared with RH9/Linux 2.4

- **Caught by:** rerunning after the timekeeping fix and observing that Emacs
  now got much farther into X setup but still spent excessive time in
  `SIGALRM`/`setitimer()` churn.
- **Symptom:** a frame began to appear, but Emacs still behaved as if its timer
  retries were far more aggressive than on the target RH9 userspace.
- **Root cause:** old Linux 2.4 i386 `ITIMER_REAL` behavior is effectively
  jiffy-based at `HZ=100`. MOS was honoring very small nonzero timer values too
  precisely. Emacs's deferred 1 ms retry path in its atimer code therefore ran
  much more aggressively on MOS than on the RH9 baseline it was built for.
- **Fix:** in [syscall_proc.c](../src/syscall/syscall_proc.c), round nonzero
  `ITIMER_REAL` values and intervals up to the next jiffy before arming the
  task alarm state.

### 3. X sockets accepted `FASYNC`/`F_SETOWN` but never delivered `SIGIO`

- **Caught by:** reading the updated Emacs-focused log once a frame existed and
  correlating it with the X input path in
  [xterm.c](../emacs-21.2-rh9-src/emacs-21.2/src/xterm.c)
  and the `SIGIO` setup in
  [keyboard.c](../emacs-21.2-rh9-src/emacs-21.2/src/keyboard.c).
- **Symptom:** Emacs mapped a window and exchanged real X traffic, but it kept
  falling back to sluggish polling / sync-style behavior instead of using its
  intended async X input path. The log showed pid `1632` installing a `SIGIO`
  handler and enabling `F_SETOWN` plus `FASYNC` on the X socket, but no
  `sig_deliver(29)` ever reached that pid.
- **Root cause:** MOS stored async ownership on the socket file descriptor, but
  the socket wakeup path never translated readability/writability changes into
  `SIGIO`. The kernel already did this for mouse input, so Emacs's X socket was
  silently missing an old BSD/Linux compatibility behavior it expected.
- **Fix:**
  - in [socket.h](../include/net/socket.h), add a back-pointer from
    `mos_sock` to the owning open file used for async notification
  - in [sock.c](../src/net/sock.c), preserve that pointer in `sock_to_fd()`
  - in [sock.c](../src/net/sock.c), teach `sock_wakeup()` to send the async
    owner `SIGIO` (or the configured alternate signal) when `FASYNC` is set

### Key lesson

Old GUI applications often fail through compatibility layering rather than one
big crash. Emacs needed:
- a monotonic `gettimeofday()`
- Linux-2.4-like `ITIMER_REAL` granularity
- actual `SIGIO` delivery on X sockets after accepting `F_SETOWN`/`FASYNC`

Once all three matched old Linux expectations closely enough, GUI Emacs worked
normally instead of stopping at a blank or half-alive frame.


## 2026-04-26 - Nautilus text preview crashed after `exit_group()` left sibling threads alive

Initial triage suggested a stack-limit or signal-trampoline bug because the
visible fault address tracked `MAX_USER_STACK`, and the first crash always
landed on a page boundary near the top of user memory. The decisive clue was
following the thread-group lifetime all the way through
`nautilus-text-view` shutdown instead of stopping at the first `SIGSEGV`.

### 1. File preview worker crashed at `befff000` / `befff002`

- **Caught by:** reading
  [out/x86/release/krn.log](../out/x86/release/krn.log) during the Nautilus
  file-preview investigation, then mapping the faulting addresses against the
  kernel signal and VDSO code.
- **Symptom:** previewing simple files such as `hello.c` in Nautilus could run
  for a long time, but opening file contents through `nautilus-text-view`
  eventually ended with `segfault: address befff000, eip befff002`.
- **What made it misleading:** `befff000` moved when `MAX_USER_STACK` changed,
  so it initially looked like the kernel was always walking one page past the
  allowed user stack floor.
- **Why that interpretation was wrong:** on MOS, the VDSO helper page is mapped
  just below the mmap/stack boundary in
  [vdso.c](../src/mm/vdso.c), so `befff000` was actually the VDSO page, and
  `eip = befff002` landed inside `__kernel_vsyscall` rather than inside a true
  stack-growth fault.

### 2. The real failure was thread-group exit, not stack growth

- **Caught by:** correlating the crash with the earlier `exit_group(0)` lines
  from the same `nautilus-text-view` instance in the syscall log.
- **Symptom:** pid `1637` called `exit_group(0)` and exited cleanly, but sibling
  thread `1638` remained alive and later faulted while returning through the
  VDSO helper. An intermediate attempt to kill siblings by queuing `SIGKILL`
  removed the VDSO crash but exposed a new debug-build hang in
  [_task_sched()](../src/ps/sched/ps_switch.c) while the scheduler tried to
  consider threads whose shared process state had already been torn down.
- **Root cause:** `sys_exit_group()` only routed through `sys_exit()`, so the
  caller destroyed process-wide state in `do_exit()` while same-`tgid`
  `CLONE_THREAD` siblings still existed. Because MOS shares the VM and higher
  process state across NPTL threads, any surviving sibling could later run with
  freed address-space or file-table state. The first visible fallout happened
  to be a VDSO fault; after a partial fix, the same bug surfaced as a scheduler
  hang.
- **Fix:**
  - in [syscall_proc.c](../src/syscall/syscall_proc.c), teach
    `sys_exit_group()` to terminate the whole thread group instead of only the
    caller
  - in [ps_syscall.c](../src/ps/ps_syscall.c), add `ps_kill_thread_group()`
    that synchronously removes same-`tgid` sibling threads from scheduler
    structures before the leader continues into `do_exit()`
  - reap those sibling threads' per-thread resources immediately, while
    explicitly preventing the shared VM from being freed multiple times

### Key lesson

For old RH9/NPTL userspace, `exit_group()` compatibility is not optional. If a
thread-group leader exits and frees shared process state before sibling threads
are actually gone, the eventual symptom may look like a random signal-frame
crash, a VDSO fault, or even a scheduler hang. When the log shows a helper
thread surviving past `exit_group(0)`, debug thread-group teardown first.

---


## 2026-04-19 - Nautilus hung in `nautilus_self_check_directory()` and later crashed in GLib allocation

This issue initially presented as a GUI application hang, but the useful clue
was that the hang consistently appeared while Nautilus was walking its
self-check startup path around directory and volume monitoring. The userspace
source and syscall log showed that the real failure was in old RH9/NPTL thread
setup, with a second bug waiting behind it in shared heap bookkeeping.

### 1. `nautilus -c` blocked during `nautilus_self_check_directory()`

- **Caught by:** reading `out/x86/release/krn.log` during investigation of
  `nautilus_self_check_directory`, then correlating that with the RH9 Nautilus
  and `gnome-vfs` sources under
  [`nautilus-2.4.2`](../nautilus-2.4.2)
  and
  [`gnome-vfs-2.4.2`](../gnome-vfs-2.4.2).
- **Symptom:** Nautilus loaded `libfam.so`, went through the file-monitor
  setup path used to watch `/etc/fstab`, issued `clone(...)`, and then the
  parent thread stopped in a futex handshake instead of continuing.
- **Root cause:** MOS installed TLS descriptors but did not consistently keep
  the task's saved user `%gs` selector in sync with that descriptor state.
  On Linux/i386, RH9 glibc/NPTL reads the current `%gs` to build the child
  `clone(CLONE_SETTLS)` descriptor. Because MOS could return to userspace with
  a stale or zero `%gs`, the newborn helper thread never completed NPTL
  startup and the parent blocked forever in the thread-creation futex.
- **Fix:**
  - in [ps_tls.c](../src/ps/ps_tls.c), update the saved user `%gs` selector
    whenever `set_thread_area()` or clone TLS installation picks a TLS slot
  - handle the RH9 case where `%gs` is LDT-backed rather than one of the Linux
    GDT TLS slots, and install equivalent child TLS for `CLONE_SETTLS`
  - stop inheriting stale occupied `tls_desc[]` state into newly created
    threads
  - clear stale TLS/LDT descriptor state across `execve()`

### 2. After the TLS fix, Nautilus moved forward and then crashed with `GLib-ERROR **: gmem.c:173: failed to allocate 32774 bytes`

- **Caught by:** rerunning after the TLS/thread fix and reading the next
  failure in `out/x86/release/krn.log`.
- **Symptom:** Nautilus no longer hung in helper-thread startup. It progressed
  into later desktop initialization, but a helper thread eventually aborted in
  GLib allocation and the main client then reported follow-on GUI protocol
  fallout such as an unexpected Xlib async reply.
- **Root cause:** MOS treated `start_brk/brk` as per-task state inside
  `user_enviroment` even for `CLONE_VM` threads. That meant multiple threads
  in one shared address space could grow or shrink the heap mapping while still
  observing different cached program-break values. Old GLib allocation paths
  then reasoned about heap state using stale `brk` bookkeeping even though the
  mappings themselves were shared correctly.
- **Fix:**
  - introduce a refcounted shared heap-state object in
    [ps.h](../include/ps/ps.h)
  - make plain `fork()` copy heap state, while `CLONE_VM` and `vfork()`
    share it
  - make `execve()` detach from any shared heap state before resetting the new
    image's `start_brk/brk`
  - update [syscall_proc.c](../src/syscall/syscall_proc.c) and `/proc` heap
    reporting paths to use the shared heap-state object

### Key lesson

For old RH9 desktop software, "threads share an address space" is not just
about sharing page tables. The kernel also has to share the higher-level
address-space bookkeeping that userspace expects to be process-wide, such as
the active TLS selector and the current program break. If those metadata stay
per-task while mappings are shared, old NPTL and GLib code will fail in ways
that look like random userspace hangs or allocation bugs.


## 2026-04-17 - SSH large output stalls until keypress (`tcp_on_sent` missing)

**Symptom**: Running `ls -alh /usr/lib` over SSH would stall partway through
the output. Pressing any key resumed it briefly, then it stalled again. Running
the same command inside the guest directly, or redirecting to a file over SSH,
worked fine.

**Root cause**: `tcp_setup_callbacks` registered `tcp_recv` and `tcp_err` but
never called `tcp_sent`. When sshd filled the lwIP TCP send buffer
(`tcp_sndbuf == 0`), `sock_tcp_stream_write` called `sock_wait` to block until
space freed. Send buffer space is freed when the remote peer ACKs data —
but that path goes through lwIP's internal ACK processing, which invokes the
`tcp_sent` callback. With no callback registered, `sock_wakeup` was never
called. The writer stayed blocked indefinitely. The only escape was a received
data packet (e.g. an SSH window-adjust or keystroke from the client), which
fired `tcp_on_recv` → `sock_wakeup` as a side effect.

**Fix**: Added `tcp_on_sent` callback (calls `sock_wakeup`) and registered it
via `tcp_sent(pcb, tcp_on_sent)` in `tcp_setup_callbacks` (`src/net/sock_cb.c`).

**Why only SSH and not file redirect**: File redirect sends no terminal output
over the TCP connection, so the TCP send buffer never fills. Inside the guest
there is no TCP socket at all.

---


## 2026-04-17 - `gnome-terminal` PTY poll wakeups could go stale

**Symptom**: Large output such as `ls -alh /usr/lib` could stall in
`gnome-terminal`, even though the same workload worked on a tty, in `screen`,
and over SSH.

**Root cause**: The PTY transport uses `cyb_notify_poll()` to wake tasks
sleeping in `poll()`. That path called [ps_put_to_ready_queue()](../src/ps/alg/ps_alg_rr.c)
unconditionally on the remembered poll task. In a fast poll-driven consumer
like old VTE, multiple PTY wakeups can arrive while the main-loop task is
already runnable or back in userspace rather than still blocked in `poll()`.
That stale wake relabeled the task as `ps_ready` again even though it was no
longer asleep, making the scheduler state timing-sensitive in exactly the way
that verbose kernel logging could mask.

**Fix**:
- in [ps_alg_rr.c](../src/ps/alg/ps_alg_rr.c), make the public
  `ps_put_to_ready_queue()` wake helper transition tasks only when they are
  still in `ps_waiting`
- leave `ps_put_to_ready_queue_unsafe()` unchanged for internal scheduler paths
  that intentionally enqueue newly created or timer-expired tasks
- keep PTY/socket/lock wakeups on the public helper so stale poll notifications
  stop perturbing runnable tasks

---


## 2026-04-14 - PTY controlling `/dev/tty` lookup broke `man`, and `strncpy()` still overflowed

This entry addresses two user-visible regressions in SSH-backed sessions:
`man ping` consistently failed once it invoked `less`, and the SSH client still
occasionally reported a bogus packet length even after the TCP tail-drop fix.
The first issue was a controlling-terminal resolution bug on PTYs; the second
was a stale libc helper overflow that could scribble on adjacent state.

### 1. `man ping` failed in SSH sessions with `Error executing formatting or display command`

- **Caught by:** reading [out/krn.log](../out/krn.log) around the `man`/`less`
  pipeline after reproducing `man ping` from an SSH-backed shell
- **Symptom:** `man` successfully formatted the page and `less` started, but
  the pager then failed while reopening `/dev/tty`, after which `man` printed
  `Error executing formatting or display command`.
- **Root cause:** MOS only resolved `/dev/tty` through the virtual-console
  table in [tty.c](../src/dev/tty.c). That works for local VTs, but an SSH
  shell runs on a PTY slave. When `less` reopened `/dev/tty` from that PTY
  session, the kernel could not map the calling process group back to the PTY
  controlling terminal, so it fell through to the wrong device path and the
  pager lost its real tty.
- **Fix:**
  - in [tty.c](../src/dev/tty.c), teach `/dev/tty` lookup to fall back from
    virtual consoles to PTY-backed controlling terminals
  - in [pty.c](../src/dev/pty.c) and [ptmx.c](../src/dev/ptmx.c), add helpers
    that reopen the PTY slave corresponding to the caller's controlling
    process group
  - in [pts_internal.h](../src/dev/pts_internal.h), expose the shared helper
    declarations needed by that lookup path
  - extend [dev_pts.sh](../test/dev_pts.sh) with a regression that creates a
    PTY session, makes it controlling via `TIOCSCTTY`, and verifies a child can
    reopen `/dev/tty`

### 2. An old `strncpy()` bug could still corrupt nearby state

- **Caught by:** reviewing low-level helpers while chasing the remaining
  intermittent SSH corruption symptom
- **Symptom:** the failure mode was sporadic and looked like memory corruption:
  the SSH client would sometimes decode nonsense packet framing even though the
  earlier TCP receive truncation bug had already been fixed.
- **Root cause:** [kstring.c](../src/lib/kstring.c) implemented `strncpy()`
  incorrectly. When the source length was at least `len`, the function copied
  `len` bytes and then still wrote a terminating NUL at `dst[len]`, one byte
  past the destination buffer. That is not POSIX `strncpy()` behavior and could
  overwrite adjacent state in exactly the kind of hard-to-reproduce way that
  shows up as protocol garbage later.
- **Fix:** in [kstring.c](../src/lib/kstring.c), make `strncpy()` follow the
  real contract: copy at most `len` bytes, pad with NULs only inside that
  range, and never append a byte past the caller-provided buffer.


## 2026-04-14 - AF_UNIX stream `SCM_RIGHTS` coalescing stalled `gnome-terminal` for 30 seconds

This issue first looked like a slow GNOME Terminal startup problem in
userspace, but the log/source correlation pointed to a kernel protocol bug.
The helper was not slow at opening PTYs; it was getting stuck because MOS
merged two adjacent fd-passing records into one stream receive.

### 1. `gnome-terminal` paused for about 30 seconds before opening

- **Caught by:** reading [out/krn.log](../out/krn.log) around the
  `gnome-pty-helper` exchange and matching it against the old VTE helper source
  in [`vte-0.11.11`](../vte-0.11.11)
- **Symptom:** the helper successfully opened `/dev/pts/0` and updated
  `utmp`/`wtmp`, but GNOME Terminal did not continue until the helper timed out
  roughly 30 seconds later and VTE fell back to plain `/dev/ptmx` allocation.
- **Root cause:** `gnome-pty-helper` sends two back-to-back `SCM_RIGHTS`
  messages, one for the PTY master and one for the slave, while VTE reads them
  with two separate `recvmsg()` calls. MOS's AF_UNIX stream path in
  [sock_un.c](../src/net/sock_un.c) drained all currently buffered bytes in one
  `recvmsg()` and then exposed all ready ancillary records at once. That let
  the first `recvmsg()` consume both one-byte helper payloads even though VTE
  only picked up one fd from that call. The second `recvmsg()` then blocked
  waiting for a second record that had effectively already been coalesced away,
  until the socket timeout fired.
- **Fix:**
  - in [sock_un.c](../src/net/sock_un.c), preserve `SCM_RIGHTS` boundaries on
    AF_UNIX stream sockets by stopping each `recvmsg()` at the next queued
    rights boundary
  - release at most one queued rights record per stream `recvmsg()` so
    back-to-back fd-passing sends are observed as separate receives
  - extend [posix_fd_pass.sh](../test/posix_fd_pass.sh) with a regression that
    sends two consecutive `SCM_RIGHTS` messages over a Unix stream socket and
    verifies they are received one-at-a-time


## 2026-04-11 - TCP receive callback dropped tail bytes, corrupting SSH streams

This issue appeared as an intermittent OpenSSH client failure rather than a
clean disconnect. The client would sometimes abort with
`Bad packet length ...`, which is a strong sign that the encrypted byte stream
itself was corrupted.

### 1. SSH occasionally failed with `Bad packet length 1349676916`

- **Caught by:** running an SSH client against the guest and seeing sporadic
  packet framing failures instead of a stable session
- **Symptom:** the client reported `Bad packet length 1349676916.` This is not
  a normal SSH protocol rejection; it means the client decoded garbage where a
  valid packet header should have been.
- **Root cause:** [sock_cb.c](../src/net/sock_cb.c) handled incoming TCP pbufs
  incorrectly when the socket receive ring did not have enough free space for
  the whole segment. `tcp_on_recv()` copied as many bytes as fit, called
  `tcp_recved()` only for those bytes, but then still freed the entire pbuf.
  That silently discarded the tail of the TCP stream. For a byte-stream
  protocol like SSH, losing even a few bytes irreversibly corrupts packet
  framing and surfaces as a bogus packet length.
- **Fix:** in [sock_cb.c](../src/net/sock_cb.c), make TCP receive all-or-nothing:
  if the receive ring cannot hold `p->tot_len`, return `ERR_MEM` and leave the
  pbuf unconsumed so lwIP can retry later. Only copy, acknowledge, and free the
  pbuf once the whole segment fits.


## 2026-04-11 - fs page-cache refill allocated before evicting, breaking `gcc` under tighter cache pressure

Initial triage suggested an `execve()` or early-userspace startup problem
because `cc1` crashed very early during `gcc -o hello hello.c` under the full
System V boot, while the same command worked in a simpler `/bin/bash` boot.
The real trigger was the filesystem page-cache replacement order.

### 1. `gcc` failed under System V boot when `PAGE_CACHE_SIZE` was small

- **Caught by:** comparing the `gcc` repro under SysV init vs direct bash boot
- **Symptom:** `cc1` segfaulted during early startup under the normal init
  boot, but succeeded when the system booted straight to bash. Increasing
  `PAGE_CACHE_SIZE` made the problem disappear.
- **Root cause:** [cache.c](../src/fs/cache.c) handled fs page-cache misses in
  the wrong order. On a miss it first called `fs_page_cache_load()`, which
  allocates a fresh user page, and only after that checked whether the cache
  was already full and evicted an old entry. Under heavy boot-time pressure,
  this meant the cache refill path still required one extra free page even when
  an evictable cache page already existed. With a smaller `PAGE_CACHE_SIZE`,
  misses and refills happened often enough for `cc1`'s early file-backed faults
  to hit this path reliably.
- **Fix:** in [cache.c](../src/fs/cache.c), evict one LRU fs page-cache entry
  before calling `fs_page_cache_load()` on a miss when `hash_size` has already
  reached `PAGE_CACHE_SIZE`.


## 2026-04-11 — PTY writes silently truncated output beyond one buffer page

Initial triage suggested an `ssh`/`select()` wakeup problem because the client
showed only the first chunk of `ls -alh /bin`. The trace was more direct: the
PTY layer was dropping data once the stream grew past the 4 KiB cyclic buffer.

### 1. `ssh` showed only the first 4096 bytes of PTY output

- **Caught by:** running `ls -alh /bin` in an SSH session and reading
  [out/krn.log](../out/krn.log)
- **Symptom:** `ls` exited normally, but the client only displayed the first
  part of the directory listing.
- **Root cause:** [pts.c](../src/dev/pts.c) used `cyb_putbuf(..., 0, 0)` in
  both `pts_master_write()` and `pts_slave_write()`, which made the cyclic
  buffer act as nonblocking. Those functions then unconditionally returned the
  caller's requested size even when `cyb_putbuf()` had accepted only a partial
  write. Once the PTY buffer filled, the tail of the stream was silently
  discarded.
- **Fix:**
  - in [pts.c](../src/dev/pts.c), honor the file's `O_NONBLOCK` state in
    `pts_master_write()` and `pts_slave_write()`
  - return the actual byte count from `cyb_putbuf()` instead of pretending the
    whole request succeeded
  - translate broken-reader cases back to `-EIO` for PTY semantics
  - keep the `ONLCR` translation path consistent with the same partial-write
    rules

### 2. The first large-transfer regression test deadlocked

- **Caught by:** running `script.posix_nonblock_ipc`
- **Symptom:** the new PTY large-write test blocked forever.
- **Root cause:** the test tried to do a blocking 8 KiB write into a 4 KiB PTY
  buffer with no concurrent reader. That is valid kernel behavior, so the test
  itself was wrong.
- **Fix:** update [posix_nonblock_ipc.sh](../test/posix_nonblock_ipc.sh) to
  fork a child writer while the parent drains the PTY master, and add
  `<sys/wait.h>` for the child exit check.


## 2026-04-11 — PTY master spurious HUP breaks `screen`; `ssh` client can't exit

Two regressions from the same select/HUP change pulled in opposite directions:
`screen` killed its window immediately after forking the shell, and `ssh` hung
forever after the remote session closed.

### 1. `screen` window died immediately after creation

- **Caught by:** running `screen` from a login shell
- **Symptom:** screen created one window, printed the shell prompt briefly, then
  exited cleanly (status 0). Kernel log showed `read(ptmx_master) = 0` at the
  first `select()` after the `fork()`.
- **Root cause (mechanism):** `select()` propagated `FS_POLL_HUP` on the ptmx
  master into `readfds` for all CHR devices. At the point screen called
  `select()`, the forked child had not yet opened the slave — so
  `cyb_writer_count(s2m) == 0` and HUP fired. Screen read 0 bytes (EOF),
  interpreted it as window exit, and tore the window down.
- **Root cause (deeper):** `cyb_writer_count(s2m) == 0` cannot distinguish
  "no slave has ever written" from "a slave opened and then exited". A plain
  flag `slave_ever_opened` would fix this, *but* `fs_chown` calls
  `vfs_open(O_RDONLY)` on the slave path, which (unconditionally) called
  `cyb_writer_open(s2m)` and would have set that flag — silently defeating the
  gate before the real shell opened the slave.
- **Fix:**
  - [pts_internal.h](../src/dev/pts_internal.h): add `slave_ever_opened` to
    `pts_pair`.
  - [fs.c](../src/fs/fs.c): change `fs_stat`, `fs_chmod`, and `fs_chown` to
    open with `O_PATH` instead of `O_RDONLY`. These functions only need inode
    access; `O_PATH` is the correct flag, and it already gates the cyclic
    buffer and slave-count operations in the slave open path.
  - [pty.c](../src/dev/pty.c) and [ptmx.c](../src/dev/ptmx.c): set
    `slave_ever_opened = 1` inside the existing `if (!(flag & O_PATH))` block —
    no new conditionals needed.
  - [pts.c](../src/dev/pts.c): gate master HUP on
    `p->slave_ever_opened && cyb_writer_count(p->s2m) == 0`.

### 2. `ssh` client could not exit after the remote session closed

- **Caught by:** running `ssh user@host`, logging out, observing the client
  hanging.
- **Symptom:** the ssh client process stayed alive indefinitely after the server
  session ended.
- **Root cause:** an earlier attempt to fix the screen regression suppressed
  CHR HUP from `readfds` in `select()`. The ssh client's controlling terminal
  is a pty slave; when the master is closed, the slave gets HUP. Without HUP
  in `readfds`, the client's `select()` returned but stdin showed nothing, so
  the client did not know to exit.
- **Fix:** revert [select.c](../src/fs/select.c) to the unified rule: any
  `FS_POLL_HUP` on a READ-subscribed fd sets `readfds`. No per-file-type
  special-casing. The spurious HUP is prevented at the source (pts.c gate),
  not masked in select.c.

---


## 2026-04-10 — Pipe and PTY EOF readiness split for `select()` vs `poll()`

This bug showed up during full System V boot rather than in the smaller POSIX
tests. `xinetd` was launched under `initlog`, and the system could burn
through nearly all RAM while boot still looked superficially alive. The real
problem was not `xinetd` itself; it was a subtle mismatch in how MOS exposed
EOF through `poll()` versus `select()` on byte-stream IPC objects.

### 1. `initlog` spun forever after daemon startup and consumed memory

- **Caught by:** booting RH9 userspace with `xinetd` enabled
- **Symptom:** `initlog` repeatedly looped on its capture pipe after
  `xinetd` daemonized, steadily growing heap via `brk()`/`mmap()` and
  eventually exhausting RAM.
- **Root cause:** the earlier SSH/self-pipe work taught anonymous pipes to
  surface EOF through readiness, but it did so too bluntly: empty closed
  streams looked like ordinary read-ready data to both `select()` and `poll()`.
  That was acceptable for EOF-observing `select()` users, but it broke
  `poll(POLLIN)` callers like `initlog`, which then woke immediately on EOF
  and spun.
- **Fix:** split EOF/HUP from ordinary read readiness:
  - in [fs.h](/home/zhengjia/project/mos/include/fs/fs.h), add an internal
    `FS_POLL_HUP` readiness bit
  - in [pipe.c](/home/zhengjia/project/mos/src/fs/pipe.c), report buffered
    data as `FS_POLL_READ` and closed-writer EOF as `FS_POLL_HUP`
  - in [pts.c](/home/zhengjia/project/mos/src/dev/pts.c), apply the same split
    to PTY master/slave poll readiness so pseudo-terminals behave consistently
    with pipes
  - in [select.c](/home/zhengjia/project/mos/src/fs/select.c), treat
    `FS_POLL_HUP` as readable when the caller asked for read readiness, so
    EOF wakeups still work for `select()`
  - in [poll.c](/home/zhengjia/project/mos/src/fs/poll.c), translate
    `FS_POLL_HUP` to `POLLHUP` instead of `POLLIN`, matching the behavior
    expected by daemon-monitoring loops

### 2. The fix had to preserve the earlier SSH `vim` and `exit` behavior

- **Caught by:** regression review immediately after the first attempt
- **Symptom:** simply hiding EOF from pipe poll readiness would have avoided
  the `initlog` spin, but it would also have regressed the SSH fixes that rely
  on `select()` waking when a pipe reaches EOF.
- **Root cause:** `select()` and `poll()` were sharing one anonymous-pipe
  readiness signal, even though user space relied on different interpretations
  of EOF.
- **Fix:** keep nonblocking `read()` semantics unchanged, and teach the common
  polling layer to expose EOF differently for the two APIs instead of removing
  EOF readiness entirely.

### 3. Regression coverage now checks the split explicitly

- **Caught by:** the need to keep both the boot fix and SSH fix at once
- **Symptom:** the previous `posix_nonblock_ipc` test covered `EAGAIN` versus
  EOF on `read()`, but it did not verify how EOF appeared through `select()`
  and `poll()`.
- **Root cause:** readiness semantics had no direct script coverage.
- **Fix:** extend [posix_nonblock_ipc.sh](/home/zhengjia/project/mos/test/posix_nonblock_ipc.sh)
  to verify:
  - `select()` reports EOF on pipes and PTYs as readable
  - `poll(POLLIN)` reports EOF via `POLLHUP` without `POLLIN`
  - ordinary nonblocking `read()` behavior remains `-EAGAIN` with live peers
    and `0` only on real EOF

### Result

- Manual verification:
  - boot with `xinetd` no longer runs `initlog` into a memory spiral
  - SSH `vim` still works
- Regression verification:
  - `./run.sh kvm test logtofile`
  - result: `rc=0`


## 2026-04-10 — SSH `vim` burst, SSH `exit` hang, and nonblocking IPC semantics

This entry began with `vim` disconnecting over SSH and exposed two separate
POSIX-visible bugs: stream sockets treated temporary TCP backpressure as a
hard write failure, and nonblocking anonymous pipes/PTYs reported EOF too
eagerly instead of `EAGAIN`. The visible symptoms initially appeared
unrelated, but both surfaced through OpenSSH.

### 1. `vim` over SSH failed on the first full-screen redraw

- **Caught by:** manual SSH session using `vim`
- **Symptom:** interactive shell traffic worked, but starting `vim` caused the
  SSH session to drop during the first large terminal repaint.
- **Root cause:** the TCP stream send path tried to enqueue one large write at
  a time and treated lwIP `ERR_MEM` as a fatal send error. Under screen-sized
  SSH packets, `tcp_write()` could temporarily reject the request because the
  send buffer had less space than the whole payload.
- **Fix:** in [sock.c](/home/zhengjia/project/mos/src/net/sock.c) and
  [sock_msg.c](/home/zhengjia/project/mos/src/net/sock_msg.c), split TCP stream
  writes into `tcp_sndbuf()`-sized chunks and retry blocking sends until space
  is available instead of failing immediately. Keep the `tcp_sent` wakeup path
  in `sock_cb.c` so blocked writers resume once ACKs free buffer space.

### 2. SSH `exit` hung because OpenSSH's self-pipe saw false EOF

- **Caught by:** manual SSH session exiting the remote shell
- **Symptom:** the shell exited, but the SSH connection stayed open and the
  server spun inside OpenSSH's `notify_done()` loop.
- **Root cause:** MOS anonymous pipe reads returned `0` for an empty
  nonblocking pipe even while writers were still open. OpenSSH drains its
  SIGCHLD self-pipe with `while (read(...) != -1)`, so receiving `0` instead of
  `-EAGAIN` made it loop forever on what looked like EOF.
- **Fix:** in [pipe.c](/home/zhengjia/project/mos/src/fs/pipe.c), make
  nonblocking reads return `-EAGAIN` when the pipe is empty but has live
  writers, and reserve `0` for true EOF only. Also update pipe poll readiness
  to report readable on buffered data or real EOF.

### 3. PTYs had the same nonblocking-read mismatch

- **Caught by:** follow-up review after fixing anonymous pipes
- **Symptom:** PTY reads ignored `O_NONBLOCK`, and empty PTYs could not cleanly
  distinguish "try again later" from real EOF.
- **Root cause:** PTY master/slave read paths always called `cyb_getbuf(..., 1,
  1)`, which forced blocking behavior and collapsed nonblocking semantics.
- **Fix:** in [pts.c](/home/zhengjia/project/mos/src/dev/pts.c), teach both
  master and slave read paths to honor `O_NONBLOCK`, return `-EAGAIN` on empty
  PTYs with a live peer, and preserve `0` for peer-close EOF. PTY poll
  readiness was also updated to treat EOF as readable.

### 4. `sendmsg()` / `recvmsg()` had duplicated logic across AF_INET and AF_UNIX

- **Caught by:** refactor after the bug fixes stabilized
- **Symptom:** the generic and AF_UNIX message paths each had their own copies
  of iovec sizing, scatter/gather ring-buffer copies, `MSG_DONTWAIT` checks,
  and cmsg append logic, which made the bug fixes easy to apply inconsistently.
- **Root cause:** message-socket helpers had grown independently in
  `sock_msg.c` and `sock_un.c`.
- **Fix:** factor shared helpers into the socket layer:
  - in [sock.h](/home/zhengjia/project/mos/include/net/sock.h) and
    [sock.c](/home/zhengjia/project/mos/src/net/sock.c), add `rx_iov_write()`,
    `rx_iov_read()`, and `rx_discard()`
  - in [sock_msg.c](/home/zhengjia/project/mos/src/net/sock_msg.c), expose
    shared helpers for iovec total length, nonblocking flag handling, and cmsg
    append
  - in [sock_un.c](/home/zhengjia/project/mos/src/net/sock_un.c), reuse those
    helpers instead of maintaining local duplicates

### 5. Regression coverage was missing for this exact class of bug

- **Caught by:** the need to make the SSH fixes stick
- **Symptom:** nothing in the script suite directly checked the crucial
  `-EAGAIN` versus EOF distinction for nonblocking pipes and PTYs.
- **Root cause:** existing tests covered basic pipes and PTYs, but not the
  OpenSSH-triggering edge case where an empty nonblocking endpoint must not
  look like EOF while the writer side is still alive.
- **Fix:** add [posix_nonblock_ipc.sh](/home/zhengjia/project/mos/test/posix_nonblock_ipc.sh),
  which verifies:
  - anonymous pipe nonblocking reads return `-EAGAIN` while the writer is open
  - anonymous pipe reads return `0` only after writer close
  - PTY master/slave reads follow the same rule

### Result

- Manual verification:
  - `vim` now works over SSH
  - remote `exit` now closes the SSH session correctly
- Regression verification:
  - `./run.sh kvm test logtofile`
  - result: `rc=0`

---


## 2026-04-10 — POSIX script-suite fixes from the long `./run.sh test` loop

This entry began as a hang in `posix_wait.sh` and ended with a full green run
of the `/proc/tests/all_script` suite. The failures were mostly not hard
crashes; they were quiet POSIX-visible mismatches that only became obvious
once the script suite was run continuously.

### 1. `waitpid()` and `sleep 0` combined into a false hang

- **Caught by:** `test/posix_wait.sh`
- **Symptom:** the suite could get stuck forever in the wait test.
- **Root cause:** two issues compounded:
  - `waitpid(..., WNOHANG)` treated `WNOHANG` like an exact option value
    instead of a bit flag
  - `nanosleep()` with zero duration blocked instead of returning immediately
- **Fix:** in `src/ps/ps_syscall.c`, make `WNOHANG` checks use
  `options & WNOHANG` and replace stale child-count checks with live scans of
  the parent/child tree; in `src/syscall/syscall_sys.c`, make zero-length
  `nanosleep()` return immediately.

### 2. Unlinking an open file broke active file descriptors

- **Caught by:** `test/posix_fcntl.sh`, `test/posix_proc.sh`,
  `test/posix_fd_pass.sh`, and related shell heredoc/temp-file use
- **Symptom:** userspace saw `Input/output error` when a temp file was unlinked
  while still open.
- **Root cause:** the ext4-backed path removed the live file immediately even
  when processes still held open descriptors to it.
- **Fix:** add delayed unlink semantics:
  - add `f_state` plus `FS_FILE_UNLINK_ON_CLOSE` in `include/fs/fs.h`
  - in `src/syscall/syscall_fs.c`, rename open unlinked files to a hidden
    tombstone and retarget all matching open file objects
  - in `src/fs/root.c`, remove the tombstone during final file release

### 3. Append, positional I/O, and inode size tracking were incomplete

- **Caught by:** `test/posix_fcntl.sh`
- **Symptom:** append writes could overwrite existing bytes, and positional I/O
  could disturb the live file cursor or backend cursor.
- **Root cause:** `pread()` / `pwrite()` used the file position incorrectly,
  and ext4 writes did not fully honor `O_APPEND` or refresh inode size.
- **Fix:** in `src/fs/fs.c`, make `pread()` and `pwrite()` operate on a
  temporary position and restore the underlying seek position; in
  `src/fs/root.c`, make append writes use EOF and update inode size on growth.

### 4. `RLIMIT_FSIZE`, `umask`, and `creat()` semantics were too weak

- **Caught by:** `test/posix_rlimit.sh` and `test/posix_umask.sh`
- **Symptom:**
  - writes past `RLIMIT_FSIZE` succeeded instead of failing with `EFBIG`
  - files and directories ignored the process umask on creation
  - `creat()` bypassed the normal open/create path
- **Root cause:** those paths were either stubs or incomplete shortcuts.
- **Fix:**
  - in `src/fs/fs.c`, enforce `RLIMIT_FSIZE` in write paths
  - in `src/syscall/syscall_proc.c`, implement `getrlimit()` by delegating to
    `ugetrlimit()`
  - in `src/fs/fs.c` and `src/syscall/syscall_fs.c`, apply umask to newly
    created files and directories
  - in `src/syscall/syscall_fs.c`, route `creat()` through the normal
    `fs_open(..., O_CREAT | O_TRUNC, mode)` path
  - in `src/fs/root.c`, explicitly fix ext4 directory mode after creation

### 5. Legacy `signal(2)` and self-signal delivery were missing edge behavior

- **Caught by:** `test/posix_signal.sh`
- **Symptom:** shell trap/self-signal cases did not behave like normal Unix
  signal delivery.
- **Root cause:**
  - syscall 48 (`signal`) was still unimplemented
  - self-directed `kill(getpid(), sig)` did not reliably deliver soon enough
    for the shell trap expectations used by the test harness
- **Fix:**
  - implement `signal(2)` in `src/ps/ps_signal.c` and register it in
    `src/syscall/syscall.c`
  - for self-directed unmasked signals, trigger delivery immediately on the
    current user return frame in `src/ps/ps_signal.c`
  - update `test/posix_signal.sh` to signal the current shell via a helper
    process using `PPID`, which is stable under the wrapper script model

### 6. Broken pipes returned `EPIPE` but did not raise `SIGPIPE`

- **Caught by:** `test/posix_pipe.sh`
- **Symptom:** shell pipelines such as `cat file | head -n 1` failed even
  though the pipe buffer code detected the broken-pipe condition.
- **Root cause:** pipe writes returned `-EPIPE` but never queued `SIGPIPE` for
  the writer.
- **Fix:** in `src/fs/pipe.c`, when `cyb_putbuf()` reports `-EPIPE`, queue
  `SIGPIPE` for the current user task.

### 7. A couple of test scripts were asserting the wrong shell semantics

- **Caught by:** `test/posix_environ.sh` and `test/posix_signal.sh`
- **Symptom:** some failures were in the tests themselves rather than the
  kernel.
- **Root cause:**
  - `${EMPTY_EXPORT:-notset}` treats an empty variable as defaulted, so it was
    not a valid check for "exported but empty"
  - `$$` under the wrapped script execution model was not the right stable way
    to target the shell process under test
- **Fix:**
  - in `test/posix_environ.sh`, check the actual variable value directly
  - in `test/posix_signal.sh`, use a helper shell that signals `PPID`

### Result

- Final verification command:
  - `./run.sh test curses logtofile`
- Final result:
  - `Total 40 Cases, 0 Failed`

---


## 2026-04-07 — Page-fault VMA lookup now uses a Linux-style `mmap_cache`

### Symptom

- The old page-fault path did a full VM-tree lookup for every fault and mixed
  two unrelated ideas under the same name: a file-page cache in
  `src/mm/pagefault.c` and Linux's actual `mmap_cache`, which is a per-task
  cache of the last `find_vma()` result.

### Root cause

- MOS had no Linux-style VMA cache at all, so repeated faults in the same VMA
  kept rewalking the interval tree.
- Stack growth logic was separate from VMA lookup instead of following the
  Linux pattern: `find_vma()`, then if the fault lands in a hole immediately
  below the grow-down stack VMA, expand that VMA and retry.
- The old in-file "mmap cache" name made the code harder to reason about,
  because it was really a custom readonly private-file page cache rather than
  a VMA cache.

### Fix

- Add `user->mmap_cache` as a Linux-style cache of the last `find_vma()`
  result.
- Add `vm_find_vma()` plus cached helpers so the fault path can distinguish
  "address is inside this VMA" from "address is in a hole but this is the next
  VMA above it".
- Rework `src/mm/pagefault.c` to use that flow for demand faults and stack
  growth.
- Remove the custom readonly `MAP_PRIVATE` file-page cache from the page-fault
  layer and move file-backed caching down into the filesystem page-cache path,
  so `read()` and mmap faults share cached file pages.
- Keep that shared file-backed cache in `src/fs/cache.c`, separate from the
  generic fs descriptor code in `src/fs/fs.c`.
- Invalidate the per-task VMA cache on `mmap`, `munmap`, `mprotect`, `fork`,
  and `exec`.

### Why this is better

- It matches the real Linux mechanism more closely, so the fault path is easier
  to compare against upstream behaviour.
- It removes both the naming collision and the extra fault-layer cache that was
  hiding the real design issue.
- It gives repeated faults in the same mapping a fast path without changing the
  existing VMA lookup, on-demand paging, and COW model, while also making file
  reads and mmap faults converge on one cache.

---


## 2026-04-07 — Shared-mapping cleanup after the boot/MM fixes

### Symptom

- After the boot fix, anonymous `MAP_SHARED` pages still lived forever once
  faulted in, and the emergency file-backed `MAP_SHARED` fallback in
  `src/mm/pagefault.c` was broader than intended.

### Root cause

- `anon_shared_map` had no lifetime tracking tied to VM-region teardown, so
  `munmap`, VMA splits/merges, and process exit never reclaimed the cached
  pages when the last sharer disappeared.
- The file-backed fallback path would also activate for files that already had
  a proper filesystem page-cache identity, even though those faults should be
  satisfied by the fs page cache or fail cleanly.

### Fix

- Add `mm_anon_shared_get()` / `mm_anon_shared_put()` and a small
  `anon_shared_refs` table in `src/mm/cache.c`.
- Wire those refs through `vm_add_map`, `vm_region_invalid`, `vm_mprotect`,
  `do_munmap`, and VMA coalescing so shared anonymous objects survive transient
  descriptor surgery but are reclaimed when the last live mapping goes away.
- Narrow the MM-cache-layer `file_shared_map` fallback so it is only used for
  `MAP_SHARED` files that cannot participate in the filesystem page cache.

### Why this is better

- Shared anonymous pages now have a real lifetime tied to live VM mappings
  instead of kernel uptime.
- The file fault path is closer to Linux: use the filesystem page cache when
  available, and keep the local fallback strictly as a compatibility escape
  hatch rather than a shadow cache.

---


## 2026-04-07 — Generic block-device lookup for ext mounts

### Symptom

- `ext4_get_sb()` in `src/fs/root.c` knew too much about storage internals: it
  stripped `/dev/` by hand, walked `hdd_partitions`, checked loop-device state
  directly, and special-cased root-device selection with `hdd_partitions[0]`.

### Root cause

- MOS had no generic block-device registry, so the ext mount path had to know
  where HDD partitions and loop devices each stored their bookkeeping.
- The public HDD partition array leaked hardware-driver internals into the fs,
  exec, proc, and `/dev` layers.

### Fix

- Add a small generic block-device registry in `src/dev/blockdev.c`.
- Register discovered HDD partitions and loop slots there, marking only
  attached/usable devices as mountable.
- Switch `ext4_get_sb()`, root mount, and exec-time remount to resolve devices
  through that registry instead of peeking at `hdd_partitions` / `loop_devs`.
- Make the HDD partition table private to `src/hw/hdd.c` and expose only
  accessors used by `/dev/hdd` and `/proc/partitions`.

### Why this is better

- The ext mount path is closer to Linux: filesystems consume a generic block
  device abstraction instead of parsing driver-private tables.
- HDD bookkeeping is no longer exported as a global array.
- Future block devices can become mount sources by registering once, without
  editing ext4 mount code again.

---


## 2026-04-07 — Bugs flushed out by the new `/proc/tests` script suite

After moving guest tests into `test/*.sh` and wiring them into
`/proc/tests/all_script`, the expanded script coverage started finding a
different class of bugs: not just kernel panics, but quiet POSIX-visible
semantic mismatches.  This section is a compact summary of the issues the new
script suite exposed and the fixes that landed.

### 1. Existing-directory `mkdir` succeeded instead of failing

- **Caught by:** `test/ret_fs.sh`
- **Symptom:** `mkdir existing-dir` succeeded silently instead of failing with
  the usual `EEXIST` behaviour.
- **Root cause:** `lwext4` directory creation checked whether the path already
  existed, but returned success instead of `EEXIST`.
- **Fix:** Return `EEXIST` from `ext4_dir_mk()` for the already-existing case.

### 2. `mkdir` with a missing parent incorrectly succeeded

- **Caught by:** `test/posix_mkdir.sh`
- **Symptom:** creating `/a/missing/child` worked when the parent directory did
  not exist.
- **Root cause:** the root filesystem `mkdir` path delegated too early into the
  ext4 layer without first validating the parent directory.
- **Fix:** validate the parent in `src/fs/root.c` before issuing the create.

### 3. `rmdir` allowed non-empty directories

- **Caught by:** `test/posix_dirs.sh`
- **Symptom:** removing a non-empty directory succeeded.
- **Root cause:** the removal path did not enforce the standard
  `ENOTEMPTY` check before unlinking the directory entry.
- **Fix:** reject `rmdir()` on non-empty directories in the root filesystem.

### 4. `O_APPEND` opens did not actually append

- **Caught by:** `test/posix_io.sh`
- **Symptom:** writes to an `O_APPEND` descriptor could overwrite existing
  contents instead of always landing at EOF.
- **Root cause:** open/write handling did not consistently move the file
  position to the current end of file for append-mode descriptors.
- **Fix:** honor `O_APPEND` in the file path so append writes are serialized at
  EOF.

### 5. Re-mounting an image as a loop device could crash the kernel

- **Caught by:** `test/loopdev.sh` and `test/ret_mount.sh`
- **Symptom:** mounting the same image again, or exercising duplicate-mount
  paths, could crash partway through the guest test run.
- **Root cause:** loop-device and mount-state handling did not reject duplicate
  mounts cleanly and had unsafe assumptions in the remount path.
- **Fix:** make duplicate mounts return `-EBUSY`, tighten loop-device handling,
  and remove the crash path.

### 6. Renaming a directory into its own descendant was not rejected

- **Caught by:** `test/posix_rename.sh`
- **Symptom:** invalid rename patterns that should fail could corrupt the tree
  or behave unpredictably.
- **Root cause:** the rename path did not detect the ancestor/descendant case.
- **Fix:** reject attempts to move a directory into its own subtree.

### 7. `/dev/pts` names were generated in hexadecimal

- **Caught by:** `test/dev_pts.sh`
- **Symptom:** PTY paths under `/dev/pts` used hex-like names instead of the
  normal decimal numbering expected by userspace.
- **Root cause:** `src/dev/ptmx.c` formatted PTY indices with `%x`.
- **Fix:** emit PTY names with `%d` so `/dev/pts/0`, `/dev/pts/1`, ... match
  normal Unix behaviour.

### 8. Read-only mmap cache eviction leaked pages and later caused crashes

- **Caught by:** heavy userspace tests such as
  `test/dev_subsystem.sh`, `test/emacs_smoke.sh`, and `test/gcc_hello.sh`
- **Symptom:** long script runs would eventually die in unrelated places after
  enough exec/mmap churn.
- **Root cause:** cached file-backed pages were dereferenced on eviction but not
  physically freed when the last reference disappeared.
- **Fix:** free the physical pages when the readonly file-cache refcount drops
  to zero, and keep cache invalidation coherent on file changes.

### 9. `proc_buf_printf()` silently truncated large generated `/proc/tests` files

- **Caught by:** the generated script infrastructure itself
- **Symptom:** larger synthetic files such as the combined test wrappers could
  hang or produce incomplete content once formatting exceeded the old 512-byte
  scratch space.
- **Root cause:** `proc_buf_printf()` relied on a fixed-size temporary buffer
  and had no way to ask `vsprintf()` for the true output length first.
- **Fix:** teach `vsprintf(NULL, ...)` / `sprintf(NULL, ...)` to return the
  exact size, then grow `proc_buf_t` accordingly before formatting.

### 10. tmpfs had several quiet POSIX mismatches

- **Caught by:** `test/posix_tmpfs.sh`
- **Symptoms:**
  - `ftruncate()` shrink/regrow could expose stale bytes
  - sparse writes past EOF could leave uninitialized data in the gap
  - `chmod`, `chown`, and `utime` on tmpfs objects were incomplete
  - directory enumeration omitted `.` and `..`
  - `unlink(dir)` / `rmdir(dir)` semantics were wrong
- **Root cause:** the initial tmpfs implementation was a narrow `/dev/shm`
  vehicle and had not been brought up to normal POSIX-visible directory and
  metadata semantics.
- **Fix:** add proper zero-fill on shrink/regrow and sparse extension, implement
  metadata updates, report `.` / `..`, compute directory link counts more
  accurately, reject directory unlink, and make `rmdir()` remove directories
  through the correct path.

### Key Lesson

Guest shell-script coverage is excellent at finding the "boring" bugs that are
easy to miss during kernel bring-up: wrong errno values, stale data exposure
after truncate, malformed `/dev` naming, directory edge cases, and metadata
operations that quietly return success without doing the right thing.  These
are precisely the bugs that make a system feel non-POSIX even when the big
features already work.

---



## 2026-04-06 — OpenSSH 3.5p1 `sshd` crash / privilege-separation bring-up

### Symptoms

- Connecting to the guest with `ssh root@10.0.5.18` initially crashed the
  per-connection `sshd` child immediately after `fork()`.
- The kernel log showed the child resetting signal handlers with
  `rt_sigaction(...)` and then dying in libc.
- After the crash was fixed, `sshd` still rejected connections because
  privilege separation reached an unimplemented `chroot(2)`.

### Root Causes

#### 1. `select(2)` overflowed heap-allocated `fd_set` buffers

MOS `do_select()` copied and cleared a full `sizeof(fd_set)` regardless of the
caller's `nfds`.  OpenSSH 3.5p1 heap-allocates only the number of bytes needed
for the requested descriptor range.  The kernel therefore wrote past the end of
that heap buffer on every `pselect()`, corrupting malloc metadata and causing
later crashes inside glibc.

#### 2. `sys_brk(0)` was not query-only

On the first call, `sys_brk()` force-mapped one page and advanced `brk` even
when userspace only wanted to query the current break.  That is not Linux
behaviour and confuses allocators that expect `brk(0)` to be side-effect free.

#### 3. `rt_sigaction` used the wrong userspace layout

glibc 2.3.2 on Linux/i386 uses a `kernel_sigaction` layout with a full
1024-bit userspace `sigset_t` in memory.  MOS had modeled the structure as only
two words of mask storage, so the kernel and glibc disagreed about the stack
object layout.

#### 4. Privilege separation required `chroot(2)` plus jail-aware path lookup

After the memory corruption issues were fixed, OpenSSH reached its privsep
preauth child and attempted to:

```text
chroot(/var/empty/sshd)
chdir(/)
setgid(...)
setuid(...)
```

`__NR_chroot` was unimplemented, and path resolution only understood the
process CWD, not a per-process jailed root.

### Fixes

**`src/fs/select.c`**

- Reject `nfds > FD_SETSIZE`
- Size all snapshots and zeroing by the actual bitset size:

```c
set_bytes = (((unsigned)(nfds ? nfds : 1) + NFDBITS - 1) / NFDBITS) *
            sizeof(fd_mask);
```

- Replace `FD_ZERO()` on output buffers with `memset(..., set_bytes)` so the
  kernel never clears beyond the caller's allocation

**`src/syscall/syscall_proc.c`**

- Make `sys_brk(0)` a pure query again
- Model `rt_sigaction` using a 32-word userspace mask (`1024 / 32`)
- Zero all returned mask words and copy only the low word that MOS currently
  implements

**`include/ps/ps.h`, `src/ps/ps_fork.c`, `src/ps/ps_syscall.c`, `src/dev/tty.c`**

- Add per-process `root_path`
- Initialize it to `"/"` for new tasks
- Copy it across `fork()`
- Free it on task reap

**`src/fs/fs.c` and `src/syscall/syscall_fs.c`**

- Teach `resolve_path()` to prepend `root_path`
- Keep `cwd` jail-relative while VFS lookups use the rooted absolute path
- Add `sys_chroot()`
- Make `chdir()` and `fchdir()` update `cwd` within the jail
- Add overflow checks so rooted path prepends cannot scribble past `MAX_PATH`

### Result

- `sshd` no longer crashes on incoming connections
- OpenSSH privilege separation now completes its `chroot()` + UID/GID drop path
- SSH negotiation proceeds normally until modern-client compatibility checks
  reject old KEX / host-key / cipher algorithms

At this point the remaining interoperability issues are protocol-age policy
differences in the client, not kernel crashes in MOS.

### Key Lesson

When a legacy daemon crashes "later" inside libc, look for earlier kernel
overwrites of user-owned variable-length buffers.  Here the visible crash was
inside malloc, but the first real bug was a kernel `select()` implementation
that assumed every userspace `fd_set` was a full `FD_SETSIZE` object.  Once the
memory corruption was gone, the next blockers were ordinary ABI completeness
items (`chroot`, path-root semantics, legacy `rt_sigaction` layout).


---


## 2026-04-06 — GNU `screen` failed with `"No more PTYs"` and later hung blank

### Symptoms

- `screen` initially failed immediately with:
  ```text
  No more PTYs.
  ```
- After the first PTY fixes, `screen` got further but stopped with:
  ```text
  TIOCPKT ioctl: Function not implemented
  ```
- After that, `screen` started and cleared the display, but the UI stayed
  blank and keyboard input appeared dead.

### Investigation

#### Step 1 — `"No more PTYs"` did not mean PTY allocation failed

The kernel log showed:

- `open("/dev/ptmx")` succeeded
- `ioctl(TIOCGPTN)` succeeded
- `stat64("/dev/pts/0")` succeeded

Old `screen` reports `"No more PTYs"` whenever its `OpenPTY()` helper fails:

```c
if ((m = ptsname(f)) == NULL || grantpt(f) || unlockpt(f)) {
    close(f);
    return -1;
}
```

So the message was just a generic userspace error path, not evidence that the
kernel had run out of PTY slots.

#### Step 2 — `grantpt()` was falling back because `/dev/pts` did not look like `devpts`

Guest `glibc-2.3.2` `grantpt()` does:

1. `ptsname()`
2. `statfs("/dev/pts/N")`
3. If `f_type` is `DEVPTS_SUPER_MAGIC` (`0x1cd1`) or `DEVFS_SUPER_MAGIC`,
   return success
4. Otherwise fall back to old legacy PTY ownership code

Our `/dev/pts` implementation had no `statfs` callback, so glibc could not
recognize it as a real `devpts` mount. That kept `screen` on the wrong
compatibility path.

#### Step 3 — slave inode metadata also had to be persistent

`/dev/pts/N` metadata was synthesized from the current task doing `stat()`
instead of stored per PTY pair. That broke the Linux/Unix98 expectation that
the slave node has stable owner/mode state after `grantpt()`.

The PTY pair needed persistent:

- slave mode
- slave uid
- slave gid
- PTY lock state

#### Step 4 — `screen` requires packet mode on the PTY master

Once `grantpt()` and `unlockpt()` worked, `screen` failed on:

```text
ioctl(fd, TIOCPKT, ...) = -ENOSYS
```

`screen` enables `TIOCPKT` on the master and aborts if it is unsupported.
So basic Unix98 PTY allocation was not enough; packet mode support was also
required.

#### Step 5 — the first `TIOCPKT` implementation was too eager

After adding `TIOCPKT`, the trace showed:

```text
write(3, "\x1b[H\x1b[J...", 12)
read(6, "\x03", 4096) = 1
pselect(...)
```

That `0x03` was generated by our own PTY code as a synthetic
`TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE` control packet after `TCFLSH`.
`screen` then sat in its event loop waiting for real PTY traffic.

Removing those fabricated flush packets let startup progress further.

#### Step 6 — the final hang was a PTY buffer lifetime bug

The remaining blank-screen hang turned out not to be packet mode at all.
The real bug was PTY buffer ownership.

The original PTY pair lifecycle used `cyb_create()` and then tried to infer
when to drop hidden reader/writer references later in `pts_pair_check_free()`.
That was incorrect:

- `pts_slave_release()` closed `p->s2m` and `p->m2s` only when
  `slave_count` reached zero
- `pts_master_release()` also closed its side
- `pts_pair_check_free()` then closed additional implicit refs again

This created two problems:

1. **Double-close / premature free**
   The shared `cy_buf` objects could be freed while pointers remained stored
   in `p->s2m` / `p->m2s`.

2. **Use-after-free in poll/read paths**
   Later code such as:
   ```c
   cyb_isempty(p->s2m)
   cyb_set_poll_read(p->s2m, task)
   ```
   could touch a freed `cy_buf`, which explained the observed hang inside the
   lock path.

### Root Causes

1. **`/dev/pts` did not implement `statfs()` with `DEVPTS_SUPER_MAGIC`**, so
   old glibc `grantpt()` took the wrong path.

2. **Unix98 slave metadata was not stored per PTY pair**, so ownership/mode
   checks did not behave like Linux `devpts`.

3. **`TIOCPKT` support was missing**, but `screen` requires it.

4. **The first packet-mode implementation injected fake flush packets**, which
   confused `screen` during startup.

5. **PTY buffer ownership was wrong**, causing freed `cy_buf` pointers to be
   reused from `p->s2m` / `p->m2s`.

### Fixes

**`src/dev/ptmx.c`**

- Added `statfs()` for `/dev/pts` returning `0x1cd1`
- Fixed `/dev/pts` directory entries to use the real PTY index
- Stored persistent slave metadata in each PTY pair
- Made slave open take explicit `cy_buf` endpoint refs
- Switched Unix98 PTY buffer allocation to `cyb_create_named()`

**`src/dev/pty.c`**

- Mirrored the same buffer lifetime fixes for BSD PTYs

**`src/dev/pts.c`**

- Added persistent slave `chmod` / `chown` support
- Implemented `TIOCSPTLCK`, `TIOCGPTLCK`, `TCFLSH`, and `TIOCPKT`
- Removed synthetic packet-mode flush notifications
- Changed `pts_pair_check_free()` to destroy only the pair-owned named buffers,
  instead of closing imaginary hidden slave refs

**`include/fs/ioctl.h`**

- Added missing `TIOCPKT` constants

### Key Lesson

PTY bugs are often two layers deep: the first visible error may be a libc
compatibility check (`grantpt`, `statfs`, ownership metadata), but once that
is fixed, interactive programs like `screen` also depend on Linux-specific
TTY behavior such as `TIOCPKT`. Most importantly, shared PTY transport objects
must have explicit ownership. If buffer lifetime is inferred indirectly from
`master_open` / `slave_count`, it is very easy to free the transport while a
poll or read path still holds the raw pointer.


---


## 2026-04-05 — `man` doesn't work (stderr opened O_WRONLY)

### Symptom

Running `man` from the shell produced no output or hung.  Other programs that
use a pager (e.g. `less`, `more`) behaved the same way.

### Root Cause

In two places the kernel opens the initial stdio file descriptors for the
first userspace process and for bash spawned from a TTY:

```c
// exec.c — kinit_userspace()
fs_open("/dev/tty1", O_RDONLY, 0);  // fd 0 — stdin
fs_open("/dev/tty1", O_WRONLY, 0);  // fd 1 — stdout
fs_open("/dev/tty1", O_WRONLY, 0);  // fd 2 — stderr  ← bug
```

`stderr` (fd 2) was opened `O_WRONLY`.  Pagers like `less` (invoked by `man`)
use fd 2 as their terminal I/O channel when stdin/stdout are redirected to a
pipe.  They call `read(2, …)` to receive keystrokes from the user.  Because
fd 2 was write-only, those reads returned `-EBADF`, and the pager could not
receive any input — appearing to hang.

The same mistake existed in `tty.c` (`tty_bash_spawner`) for consoles spawned
after VT switch.

### Fix

Open fd 2 as `O_RDWR` so the TTY file descriptor is usable for both reading
and writing, matching real Linux behaviour:

```c
fs_open("/dev/tty1", O_RDWR, 0);  // fd 2 — stderr
```

Changed in `src/elf/exec.c` and `src/dev/tty.c`.

### Key Lesson

The TTY fd used as the controlling terminal must be `O_RDWR`.  Even though
`stderr` is conventionally write-only for application output, the kernel-side
fd must allow reads because pagers and interactive programs re-use fd 2 as
their terminal input channel when stdin is a pipe.

---


## 2026-04-05 — xinetd crash + "not enough memory" (ugetrlimit / setrlimit / fs_close)

### Symptoms

- `/usr/sbin/xinetd` segfaults immediately after startup:
  ```
  [899]: segfault: error code 4, address 0, eip 806114f, cmd /usr/sbin/xinetd
  ```
  The crash is a NULL dereference inside xinetd's own code, not in a library.

- Some other services log `"not enough memory"` at startup.

- Both symptoms appeared together and seemed contradictory: the value that
  fixed the crash caused the OOM, and vice versa.

### Investigation

#### Step 1 — identify which resources are queried

The kernel log showed `ugetrlimit(3)`, `ugetrlimit(4)`, and `ugetrlimit(7)`
being called. Resources 3 and 4 were returning `RLIM_INFINITY` (the default
fallback). Resource 7 (RLIMIT_NOFILE) was the only one with special handling.

#### Step 2 — narrow down by varying the return value

| `rl[0]` (cur)   | `rl[1]` (max)   | xinetd result | other services      |
| --------------- | --------------- | ------------- | ------------------- |
| `MAX_FD`(341)   | `RLIM_INFINITY` | segfault      | ok                  |
| `RLIM_INFINITY` | `MAX_FD`(341)   | **no crash**  | "not enough memory" |
| `RLIM_INFINITY` | `RLIM_INFINITY` | **no crash**  | "not enough memory" |

The value of `rl[0]` (soft limit / `rlim_cur`) was the decisive variable.

#### Step 3 — understand xinetd's startup sequence

Reading xinetd's source (`init.c`, `set_fd_limit()`):

```c
getrlimit(RLIMIT_NOFILE, &rl);
maxfd = rl.rlim_max;
if (rl.rlim_max == RLIM_INFINITY)
    rl.rlim_max = FD_SETSIZE;   // cap to 1024
if (rl.rlim_max > FD_SETSIZE)
    rl.rlim_max = FD_SETSIZE;
rl.rlim_cur = rl.rlim_max;
setrlimit(RLIMIT_NOFILE, &rl);  // sets both to min(rl_max, 1024)
ps.ros.max_descriptors = rl.rlim_max;
```

Then:
```c
for (fd = 3; fd < ps.ros.max_descriptors; fd++)
    Sclose(fd);   // close all fds above stderr
```

#### Step 4 — find why rl_cur matters

glibc reads `RLIMIT_NOFILE` **once at startup** (before `main()`) and uses
`rl_cur` to size its internal fd table. With `rl_cur = 341` glibc allocates
a 341-slot table. Then xinetd calls `setrlimit({1024, 1024})` which our
kernel was **silently ignoring** (setrlimit was a no-op). xinetd believed
`max_descriptors = 1024` and its close loop ran up to fd 1023. When fd 342
was accessed, glibc's table was out of bounds → NULL dereference → segfault.

With `rl_cur = RLIM_INFINITY`, glibc takes a different code path and does
not allocate a fixed-size table, so no out-of-bounds access.

#### Step 5 — understand the "not enough memory" service

That service does roughly:
```c
getrlimit(RLIMIT_NOFILE, &rl);
table = malloc(rl.rlim_cur * sizeof(ptr));
if (!table) log("not enough memory");
```
With `rl_cur = RLIM_INFINITY = 0xFFFFFFFF`, `malloc` fails. On real Linux,
`RLIMIT_NOFILE` defaults to `{1024, 1024}` — not `RLIM_INFINITY` — so this
never happens there.

#### Step 6 — the real Linux default is {1024, 1024}

Linux kernel default for `RLIMIT_NOFILE` is soft=1024, hard=1024.
With `{1024, 1024}`:
- glibc allocates a 1024-slot table
- xinetd gets `rl_max = 1024`, no capping needed, `max_descriptors = 1024`
- xinetd's `setrlimit({1024, 1024})` matches what we already return
- The table size and `max_descriptors` agree → no out-of-bounds
- `malloc(1024 * sizeof(...))` succeeds → no OOM

#### Step 7 — setrlimit must track values per-process

Even with the correct default, `setrlimit` must actually store the new
value. A child process forked after xinetd calls `setrlimit({1024, 1024})`
should inherit `{1024, 1024}`, not the default. Without tracking, children
read the initial default and glibc allocates the wrong table size again.

#### Step 8 — fs_close returning wrong errno

`fs_close` was returning `-ENOENT` for fds that are not open. Linux returns
`-EBADF` for all invalid `close()` calls. xinetd's close loop is:
```c
if (Sclose(fd) && errno != EBADF) { exit(1); }
```
`ENOENT ≠ EBADF` would cause a premature `exit(1)`. This is correct POSIX
behavior that was simply wrong in our implementation.

### Root Causes

1. **`RLIMIT_NOFILE` default was not `{1024, 1024}`** — we returned either
   `MAX_FD` (too small, glibc table mismatch) or `RLIM_INFINITY` (services
   OOM on malloc). The Linux default of `{1024, 1024}` satisfies both.

2. **`setrlimit` was a no-op** — it did not store the new limit. Children
   would inherit the initial default instead of what the parent set.

3. **`fs_close` returned `-ENOENT` for unopened fds** instead of `-EBADF`.

### Fixes

**`include/ps/ps.h`** — add per-process rlimit storage:
```c
#define RLIM_NLIMITS 16
#define RLIM_INFINITY 0xFFFFFFFFu

typedef struct { unsigned long rlim_cur, rlim_max; } rlimit_t;

struct _task_struct {
    ...
    rlimit_t rlimits[RLIM_NLIMITS];
};
```

**`src/ps/ps_fork.c`** (`_ps_create`) — initialize defaults:
```c
for (int i = 0; i < RLIM_NLIMITS; i++)
    task->rlimits[i] = {RLIM_INFINITY, RLIM_INFINITY};
task->rlimits[3] = {USER_STACK_PAGES * PAGE_SIZE, ...}; // RLIMIT_STACK
task->rlimits[4] = {0, 0};                              // RLIMIT_CORE
task->rlimits[7] = {1024, 1024};                        // RLIMIT_NOFILE
```
Fork inherits automatically because `fork_alloc_child` does `*task = *cur`.

**`src/syscall/syscall_proc.c`** — wire up reads and writes:
```c
int sys_ugetrlimit(int resource, void *limit) {
    task_struct *cur = CURRENT_TASK();
    unsigned long *rl = limit;
    if (rl && resource >= 0 && resource < RLIM_NLIMITS) {
        rl[0] = cur->rlimits[resource].rlim_cur;
        rl[1] = cur->rlimits[resource].rlim_max;
    }
    return 0;
}

int sys_setrlimit(int resource, void *limit) {
    task_struct *cur = CURRENT_TASK();
    unsigned long *rl = limit;
    if (rl && resource >= 0 && resource < RLIM_NLIMITS) {
        cur->rlimits[resource].rlim_cur = rl[0];
        cur->rlimits[resource].rlim_max = rl[1];
    }
    return 0;
}
```

**`src/fs/fs.c`** (`fs_close`) — return correct errno:
```c
if (cur->fds[fd].used == 0)
    return -EBADF;   // was -ENOENT
```

### Key Lesson

When a service crashes at address 0 right after `execve`, check what value
glibc uses from `ugetrlimit` at startup — glibc sizes internal tables from
`rl_cur` before `main()` runs, and that sizing is permanent. Any subsequent
`setrlimit` call does not resize the already-allocated table. The kernel's
default must match the real Linux default (`{1024, 1024}` for RLIMIT_NOFILE)
so all userspace assumptions are met from the start.
