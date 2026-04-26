# GUI Debug Journal

## 2026-04-26

Status at stop:
- GUI `emacs` now works.
- The original "no Emacs window" failure is gone.
- The intermediate "window frame appears but content never becomes usable"
  phase is also gone.
- `xclock` was already rendering correctly during this round, which helped
  separate generic X rendering from Emacs-specific compatibility issues.
- `xterm` was still slower than ideal during investigation, but Emacs itself
  is no longer blocked.

Fixes completed in this round:
- Fixed PIT-based wall-clock sampling so `gettimeofday()` no longer jumps
  backward when the timer IRQ pending check races with the latched counter:
  - added previous-sample tracking in [time.c](../src/hw/time.c)
  - only compensate for a missing tick when the PIT counter proves a wrap in
    the same `tickets` epoch
- Adjusted `ITIMER_REAL` semantics to better match RH9/Linux 2.4 behavior:
  - rounded nonzero timer values and intervals up to the next jiffy in
    [syscall_proc.c](../src/syscall/syscall_proc.c)
  - this matches the coarser `HZ=100` behavior old Emacs expects for its
    `SIGALRM` retry path
- Added async `SIGIO` delivery for sockets:
  - stored the owning open file on each socket in
    [socket.h](../include/net/socket.h)
  - wired `sock_to_fd()` to preserve that link in
    [sock.c](../src/net/sock.c)
  - taught `sock_wakeup()` in [sock.c](../src/net/sock.c) to send the
    configured async signal to the `F_SETOWN` owner when `FASYNC` is enabled

Observed failure progression during this round:
- Initially, GUI Emacs did not show at all. The log showed a `SIGALRM` /
  `setitimer()` / `gettimeofday()` storm while Emacs was still bringing up its
  X connection.
- After the monotonic-clock fix, Emacs advanced much farther and began mapping
  a real X window.
- After the `ITIMER_REAL` granularity fix, the window appeared more
  consistently, but content still did not fully settle.
- The decisive later clue was that Emacs enabled `F_SETOWN` and `FASYNC` on
  its X socket and installed a `SIGIO` handler, yet the log never showed
  `sig_deliver(29)` for the Emacs pid. That pointed to a missing kernel async
  socket wakeup path rather than a pure X drawing problem.
- Once socket `SIGIO` delivery was added, GUI Emacs became usable.

Working conclusion:
- The Emacs failure was not one X11 rendering bug. It was a compatibility
  stack:
  - non-monotonic wall clock broke atimer scheduling
  - too-fine `ITIMER_REAL` semantics amplified Emacs retry behavior beyond what
    RH9/Linux 2.4 would do
  - missing async `SIGIO` delivery on sockets prevented Emacs from using its
    intended X input path
- With those three fixed, MOS is significantly closer to the timing and async
  I/O expectations of old RH9 desktop software.

Suggested next step:
- Return to the remaining GUI polish issue around `xterm` slowness, which now
  looks independent from the Emacs bring-up path that was blocking this round.

## 2026-04-19

Status at stop:
- `nautilus -c` now runs through `nautilus_self_check_directory()` instead of hanging in early startup.
- The old Nautilus/FAM-triggered thread startup block is fixed.
- Nautilus also no longer dies later in GLib heap allocation once helper threads are running.

Fixes completed in this round:
- Fixed i386 TLS setup so `set_thread_area()` updates the saved user `%gs` selector alongside the descriptor install.
- Fixed `CLONE_SETTLS` for the RH9/NPTL case where the parent thread is still using an LDT-backed `%gs`, so child threads inherit valid TLS instead of stalling in the startup futex handshake.
- Fixed TLS slot lifetime issues:
  - `set_thread_area()` now allows empty descriptors to clear slots
  - new threads no longer inherit stale occupied `tls_desc[]` state
  - `execve()` clears old TLS/LDT descriptor state before a new image starts
- Reworked heap bookkeeping so `CLONE_VM` tasks share `start_brk/brk` through a shared heap-state object instead of keeping per-task copies that drift apart.
- Updated `/proc` heap reporting and teardown paths to use the shared heap-state model.
- Removed the temporary TLS/clone debugging logs after the fix was verified.

Observed failure progression during this round:
- The original Nautilus failure looked like a hang in `nautilus_self_check_directory()`, but the real block was a helper-thread startup futex wait after GNOME VFS tried to monitor `/etc/fstab`.
- Once TLS setup was corrected, the hang moved forward into a later `GLib-ERROR **: gmem.c:173: failed to allocate 32774 bytes`.
- That later allocation failure came from `CLONE_VM` threads sharing mappings but not sharing `brk` bookkeeping.
- After making heap state shared for shared-VM tasks and private again across `fork()`/`execve()`, Nautilus completed the problematic startup path successfully.

Current working conclusion:
- The Nautilus bring-up problem was not a missing FAM daemon by itself; missing FAM only exercised an NPTL thread path that exposed MOS kernel bugs.
- Two kernel-level compatibility gaps were involved:
  - Linux/i386 TLS semantics for NPTL thread startup
  - shared heap bookkeeping semantics for `CLONE_VM`
- With both fixed, the GUI stack is materially closer to RH9/NPTL-compatible desktop behavior.


## 2026-04-12

Status at stop:
- The graphical desktop is now visible.
- `startx` no longer dies in the old X loader/config/input paths.
- GNOME session startup now gets through X, ICE, Bonobo activation, and into the desktop bring-up sequence.

Fixes completed since the last journal update:
- Fixed stale user PTE teardown in `mm_unmap_page()` so mappings backed by non-allocator physical pages are actually removed.
- Fixed `vm_add_map()` overlap replacement so `MAP_FIXED` only unmaps the overlapping slice instead of destroying entire neighboring file-backed regions.
- Fixed copy-on-write/write-protect handling to operate on page-aligned fault addresses.
- Added minimal scheduler syscall support required by glib/pthreads:
  - `sched_getparam`
  - `sched_getscheduler`
  - `sched_setparam`
  - `sched_setscheduler`
  - `sched_get_priority_max`
  - `sched_get_priority_min`
  - `sched_rr_get_interval`
- Normalized trailing slashes in the ext4/lwext4 full-path path so operations like `mkdir("/root/.gnome2/")` behave like Linux.
- Fixed `readv()`/`writev()` so stream/socket users get single vectored operations instead of per-iovec blocking behavior.
- Added valid socket inode `getattr` support so `fstat64()` works on AF_UNIX socket descriptors used during ICE/session setup.
- Fixed AF_UNIX stream write semantics:
  - blocking writes now wait for peer buffer space
  - nonblocking writes return `-EAGAIN` instead of stalling incorrectly
  - stream `sendmsg()` no longer returns spurious `-ENOBUFS` on blocking paths
- Fixed `select`/`__newselect` userspace ABI handling to use `struct timeval` rather than `struct timespec`.
- Fixed socket poll write-readiness so AF_UNIX and TCP stream sockets only report writable when the peer/send buffer actually has space.
- Fixed file-backed `mmap()` validation:
  - reject invalid fds for non-anonymous mappings
  - reject directory-backed `mmap()` instead of allowing a later SIGSEGV
  - preserve the historical `fd == -1` anonymous behavior used by MOS exec stack setup
  - continue allowing character-device mappings such as `/dev/mem`, which XFree86/VESA needs

Observed failure progression during this round:
- The old `/dev/psaux` core-pointer failure was replaced by successful X input setup once the config matched `/dev/input/mice`.
- The dynamic linker crash in `ld-2.3.2.so` while loading `libnss_files.so.2` was eliminated by the VM unmap/overlap fixes.
- The GThread abort on `pthread_getschedparam()` moved the failure forward into GNOME session startup once scheduler syscalls existed.
- The `/root/.gnome2/` creation failure was eliminated by path normalization for trailing slashes.
- The "big X cursor, nothing more" phase was narrowed to IPC/event-loop behavior, then improved by the AF_UNIX, vectored I/O, socket `fstat64()`, and `select()` fixes.
- A later fontconfig crash came from allowing `mmap()` on a directory (`/usr/share/fonts/default`); rejecting directory-backed `mmap()` removed that crash path.
- A temporary regression that blocked `/dev/mem` mappings caused `VESA(0): Cannot map SYS BIOS`; allowing character-device `mmap()` restored that path.

Current working conclusion:
- The GUI bring-up path is now substantially functional.
- The remaining work, if any, should be treated as incremental desktop polish or narrower application/runtime issues rather than foundational X startup failure.

Cleanup note:
- Temporary investigation-only loader/page-fault tracing added during the earlier `_dl_lookup_symbol` and `libnss_files` debugging has already been removed.
- No additional one-off GUI debugging stubs were found in the current tree during this cleanup pass beyond the normal configurable syscall/kernel logging already used by the project.



## 2026-04-12

Status at stop:
- GUI mouse input now works correctly inside X.
- The PS/2 mouse path is no longer a fake `/dev/input/mice` stub; it is backed by a real hardware driver under `src/hw`.
- Pointer movement was verified after fixing both probe-time compatibility and runtime async delivery issues.

Fixes completed in this round:
- Implemented a real PS/2 mouse driver in `src/hw/mouse.c` with i8042 auxiliary-port init, IRQ12 handling, DSR-based byte draining, packet assembly, and IMPS/2 wheel-mode negotiation.
- Completed `src/dev/mouse.c` as a proper `/dev/input/mice` wrapper over the hardware driver instead of maintaining a separate fake device implementation.
- Added PS/2 command handling needed by X probe logic, including reset, identify, sample-rate, resolution, status, read-data, defaults, and enable/disable reporting behavior.
- Added async mouse notification support used by X at runtime, including `fcntl(F_SETOWN/F_GETOWN/F_SETSIG/F_GETSIG)` state on open files and `SIGIO` delivery when new mouse bytes arrive.
- Fixed a runtime input stall where X stopped receiving wakeups after probe by honoring async notification ownership on the mouse fd.
- Fixed a second async-design bug where only one global mouse file was tracked, so unrelated opens/closes could break X input delivery.
- Fixed a follow-up deadlock in the packet completion path by moving async queue/notify work out from under `mouse_state_lock`.
- Removed temporary mouse debugging stubs after the input path was confirmed working.

Working conclusion:
- XFree86 expects `/dev/input/mice` to behave like a real PS/2-compatible endpoint during probe and like an async signal-driven input source afterward.
- The remaining GUI bring-up work is no longer blocked on mouse input; MOS now has stable end-to-end pointer delivery from IRQ12 through `/dev/input/mice` into X.


## 2026-04-09

Status at stop:
- `startx` now reaches a visible X screen.
- X, `twm`, and `xterm` can all start under the current kernel.
- A large black X cursor is visible, which confirms the graphics VT and framebuffer present path are alive.
- The original X server death on `SIGALRM` is fixed.
- The remaining blockers are helper-process crashes in `xkbcomp` and `tradcpp0`/`cpp`, which degrade XKB and `xrdb` resource loading but do not prevent the X session itself from coming up.

Fixes completed in this round:
- Fixed `fork()` so child tasks do not inherit an already-armed real-time alarm from the parent.
- Corrected the `rt_sigaction` userspace ABI layout to decode `handler, mask, flags, restorer` in the order Linux/i386 userspace expects.
- Implemented `rt_sigreturn` support and an RT signal frame path for `SA_SIGINFO` handlers.
- Fixed Linux/i386 signal delivery selection so MOS uses the RT frame only for `SA_SIGINFO`, not merely because a handler was installed via `rt_sigaction`.
- Replaced the earlier synthetic VDSO-only legacy signal return path with the classic Linux/i386 inline on-stack `sigreturn` trampoline, which stopped X from returning into invalid code after `SIGALRM`.
- Fixed legacy `sigreturn` context restore so the saved user data segments are restored alongside the general-purpose registers.
- Moved signal delivery and signal-related syscalls into `src/ps/ps_signal.c`, leaving `syscall_proc.c` for non-signal process syscalls.
- Extended `/dev/input/mice` compatibility:
  - added tty-style ioctls used by X mouse probe paths
  - implemented basic PS/2 command responses for reset, identify, sample-rate, resolution, enable, disable, and poll traffic
  - seeded initial mouse packets so X sees a live device instead of an inert endpoint
- Fixed `/dev/tty` and PTY startup behavior enough for `xterm` to open its controlling terminal, allocate `/dev/ptmx`, and use `/dev/pts/0`.
- Re-enabled periodic graphics presentation from the timer path so framebuffer writes on graphics VTs are pushed to the visible display.
- Tightened AF_UNIX readiness handling:
  - write readiness for connected Unix sockets now depends on actual peer receive space
  - readers wake blocked writers after draining data
  - socket wakeups now avoid re-queueing tasks that are not actually sleeping
- Added verbose levels so heavy syscall tracing can be separated from lighter informational logging while debugging GUI startup.
- Cleared the direction flag on syscall entry and on the fresh exec-to-user path so kernel/user string operations do not inherit a stale `DF=1`.
- Stopped fresh `execve` entry from inheriting arbitrary kernel `EFLAGS`, and sanitized special control bits such as `RF`/`NT`/`AC`/`VM` before returning to ring 3 while still restoring the task's requested IOPL.

Working conclusion:
- The project moved from early startup failures into a working X session with remaining helper-process incompatibilities.
- `xinit` is no longer blocked waiting for the X server; it is attached to a live session whose clients keep running normally.
- The old X crash was caused by MOS signal ABI mismatches in the `SIGALRM` path used by the X server smart scheduler.
- The current unresolved failures are:
  - `xkbcomp`, which now crashes during early Xlib/glibc locale setup before it reaches its real keymap compilation logic
  - `tradcpp0`/`cpp`, which still fail under `xrdb`, leaving `.Xresources` preprocessing incomplete
- Current evidence rules out missing user stack mappings and bad user segment selectors for those helper crashes; the remaining issue appears to be lower-level user execution-state corruption on some helper process paths.

Suggested next step:
- Resume from the helper-process crash path rather than the old X-server timeout path, starting with `xkbcomp`'s early `XkbOpenDisplay` / glibc locale initialization and the corresponding low-level user-context restore/return machinery in MOS.


## 2026-04-08

Observed behavior today:
- The earlier AF_UNIX theory was refined. `unix_ns` being empty is now believed to be a consequence, not the root cause.
- `out/krn.log` shows `/usr/X11R6/bin/X` does create the local X socket:
  - `socket(domain=1, type=1, protocol=0)`
  - `bind(fd=3, addr=..., addrlen=19)`
  - `listen(fd=3, backlog=128)`
- So the pathname listener for `/tmp/.X11-unix/X0` exists at least briefly.
- The stronger finding is that `/usr/X11R6/bin/X` exits through the user-mode general-protection path in [int.c](src/int/int.c) line 151.
- That means X takes a user-space `#GP`, MOS kills it with `sys_exit(-EFAULT)`, and only after that do later client `connect()` attempts see `unix_ns` empty and return `ECONNREFUSED`.

Startx-related VM/MMIO bugs found and fixed today:
- The framebuffer mapping path was confirmed to be `/dev/mem` MMIO, not normal RAM-backed file cache. The relevant fault path in [pagefault.c](src/mm/pagefault.c#L140) already maps `/dev/mem` directly with `mm_map_page_io()` and `PAGE_ENTRY_CD`, which is correct for VRAM/MMIO.
- A later `fork()` crash showed a child PTE value of `0xFD000077` in [ps_fork.c](src/ps/ps_fork.c#L190). That decodes to physical `0xFD000000` with present, writable, user, and cache-disabled bits, i.e. the VESA/VMware linear framebuffer BAR.
- The bug in `copy_one_pte()` was that it assumed every present user PTE belonged to allocator-managed RAM and unconditionally fed the derived page index into `phymm_reference_page()`.
- Fixed [ps_fork.c](src/ps/ps_fork.c#L200) so `copy_one_pte()` clones `/dev/mem` mappings directly, and more defensively skips `phymm_reference_page()` for any attached physical page outside `[phymm_begin, phymm_end)` or marked `PHYMM_RESERVED`.
- A second crash appeared later in `vm_flush_region_cb()` / `vm_flush_dirty_region()` while tearing down or flushing X mappings. The failing virtual address was `0x4028A000`, but its attached page index again resolved to `1036288`, which is physical `0xFD000000`.
- The bug in `vm_flush_dirty_region()` was the same assumption in a different place: it treated every `MAP_SHARED` file-backed mapped page as if it had a valid `phymm` page and could be checked with `phymm_is_dirty()` and written back like normal cache-backed file data.
- Fixed [mmap.c](src/mm/mmap.c#L654) so `vm_flush_dirty_region()` skips `/dev/mem` mappings entirely and also ignores attached pages that are outside allocator-managed RAM or marked `PHYMM_RESERVED`.

Refined conclusion:
- Today exposed two separate VM-layer assumptions that were invalid for `startx` framebuffer mappings.
- `fork()` assumed every present user PTE was backed by `phymm`.
- shared-file dirty flush assumed every attached mapped page was backed by `phymm`.
- Both assumptions were false for the X framebuffer mapping at physical `0xFD000000`.
- The AF_UNIX `ECONNREFUSED` symptom still looks downstream of X crashing, but the MMIO/framebuffer crashes in fork and dirty-flush paths are now guarded and should no longer take the kernel down during `startx`.

Suggested next step:
- Resume by identifying what user-mode operation in `/usr/X11R6/bin/X` triggers the `#GP` shortly after the late XKB/keyboard path, now that the framebuffer/MMIO crashes in the VM layer are covered.


## 2026-04-07

Status at stop:
- `startx` still does not complete.
- `tty1` remains responsive.
- `xinit` now reports `Connection refused (errno 111): unable to connect to X server`.

Fixes completed in this round:
- Added Linux VT/console ioctls and basic VT state handling so X can allocate and switch VTs instead of failing at `xf86OpenConsole`.
- Implemented missing tty compatibility calls used during X startup, including `TIOCNOTTY`, `KDSETMODE`/`KDGETMODE` state handling, LED/repeat keyboard ioctls, and related tty compatibility behavior.
- Stopped the tty text layer from redrawing over a VT while it is in `KD_GRAPHICS`.
- Added minimal `/dev/mem` support so XFree86 `int10` and framebuffer setup can access physical memory as expected.
- Added minimal SysV shared memory support through `ipc(2)` for the old XFree86 `int10` path.
- Implemented vm86/VBE compatibility shims needed by the `vesa` driver, including controller info, mode info, mode set/get, save/restore query, scanline, palette/DAC, pixel clock, DDC fallback, and VMware SVGA mode hookup.
- Fixed VBE data layout issues that previously corrupted userspace memory and prevented BIOS/VBE probing from succeeding.
- Enabled large framebuffer `mmap()` so X can map the 16 MiB linear framebuffer.
- Added root port-I/O compatibility for X startup and later moved it from the unsafe `IOPL=3` shortcut to a TSS I/O-bitmap approach while keeping user IOPL cleared.
- Added a general-protection fault handler so user-mode I/O and compatibility mistakes fail visibly instead of silently wedging the kernel.
- Added `/dev/input/mice` compatibility so the mouse device can open and initialize.
- Fixed `rt_sigaction` userspace ABI decoding and several signal/process-control issues (`kill`, signal inheritance/reset across `execve`, and related startup handshake behavior).
- Fixed AF_UNIX listener readiness handling so queued local X connections are visible to the server-side socket layer.
- Implemented real `readv()` and corrected nonblocking socket read behavior.
- Implemented `setitimer(2)` / `getitimer(2)` support for `ITIMER_REAL`.
- Fixed `nanosleep()` interruption behavior to use the normal wait path correctly.
- Fixed a real kernel deadlock in the spinlock slow path by removing `HLT` while spinning with interrupts disabled.

Net effect:
- XFree86 now gets far deeper into startup than at the beginning of this effort.
- The failure has moved from immediate early-console and BIOS compatibility errors to a late startup / server-availability problem.

Current observable state:
- The X server creates and listens on `/tmp/.X11-unix/X0`.
- A client connection to `X0` is observed in the kernel log.
- Despite that, the server still does not complete startup, and userland later sees `ECONNREFUSED`.

Working conclusion:
- General system interrupt handling appears okay because `tty1` continues running normally.
- The remaining issue is still in the late GUI/X startup path, after socket publication and before the server becomes usable for `xinit`.

Suggested next step:
- Resume from the late X server control flow around the final wait/sleep/listener path and correlate it with the later `ECONNREFUSED`.

