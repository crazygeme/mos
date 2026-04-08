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
