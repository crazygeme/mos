# GUI Debug Journal

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

## 2026-04-08

Checkpoint for next session:
- The earlier AF_UNIX theory was refined. `unix_ns` being empty is now believed to be a consequence, not the root cause.
- `out/krn.log` shows `/usr/X11R6/bin/X` does create the local X socket:
  - `socket(domain=1, type=1, protocol=0)`
  - `bind(fd=3, addr=..., addrlen=19)`
  - `listen(fd=3, backlog=128)`
- So the local pathname listener for `/tmp/.X11-unix/X0` is created at least briefly.
- The stronger finding is that process `/usr/X11R6/bin/X` exits through the user-mode general-protection path in [int.c](/home/zhengjia/project/mos/src/int/int.c) at line 151.
- That means X takes a user-space `#GP`, MOS kills it with `sys_exit(-EFAULT)`, and only after that do later client `connect()` attempts see `unix_ns` empty and return `ECONNREFUSED`.

Current working conclusion:
- Root cause is likely the user-mode `#GP` in X, not AF_UNIX namespace lookup itself.
- The empty `/tmp/.X11-unix/X0` listener state is likely post-crash cleanup after X exits.

Suggested next step:
- Resume by identifying what user-mode operation in `/usr/X11R6/bin/X` triggers the `#GP` shortly after the late XKB/keyboard path.
