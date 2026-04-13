# Bringing Up X On Your Own Kernel

This document is a practical checklist for anyone who wants to boot an old Linux/XFree86-style graphical stack on a homegrown x86 kernel.

The important mindset is that X bring-up is not only a graphics-driver problem. A server such as `XFree86` or early `Xorg` depends on a wide compatibility surface:

- virtual terminals and controlling-terminal behavior
- framebuffer or card access
- low-level x86 compatibility paths such as VBE and port I/O
- input devices that behave like Linux devices
- local Unix sockets for client connections
- signal, timer, IPC, and `mmap()` behavior close enough to Linux/glibc expectations

If any one of those contracts is missing, `startx` may fail even though your kernel can already run shells, ELF binaries, and ordinary console programs.

## 1. Define What "X Is Up" Means

Before implementation, decide what success means.

A useful target is:

- the X server can start without immediately exiting
- it can claim a VT and switch it into graphics mode
- it can publish `/tmp/.X11-unix/X0`
- `xinit` can connect to that socket
- simple clients such as `xterm` can create windows
- keyboard and mouse input work after startup, not just during probe

That definition keeps you focused on end-to-end behavior instead of only on whether the server binary executes.

## 2. Virtual Terminals And Console Control

An old X server usually expects to own a real text console before it switches into graphics mode.

Your kernel should provide:

- virtual terminals or an equivalent console abstraction
- session and foreground process-group tracking
- controlling-terminal support
- VT switching support
- keyboard/display mode switching between text and graphics

At minimum, plan for compatibility with these Linux-style interfaces:

- `TIOCSCTTY`, `TIOCNOTTY`
- `TIOCGPGRP`, `TIOCSPGRP`
- `VT_OPENQRY`
- `VT_GETMODE`, `VT_SETMODE`
- `VT_GETSTATE`
- `VT_ACTIVATE`, `VT_WAITACTIVE`, `VT_RELDISP`
- `KDGETMODE`, `KDSETMODE`

The critical rule is simple: once the server switches a VT into graphics mode, your text console renderer must stop painting over that VT. Cursor redraws, console scrollback flushes, or printk output that still touches the visible graphics VT will corrupt the screen.

## 3. Graphics Device Access

X must be able to discover a usable graphics mode and write pixels to the framebuffer.

Your kernel should provide:

- a display path that the chosen X driver understands
- a stable way to identify the framebuffer physical address and size
- user-visible mapping of the linear framebuffer
- enough framebuffer size allowance for large mappings

For old XFree86-style bring-up on x86, the usual expectation is:

- the server probes VBE or a simple card-specific interface
- it maps VRAM through `/dev/mem`
- userspace stores go directly to the framebuffer BAR

That means your VM layer must treat MMIO and VRAM differently from ordinary file-backed pages. If you run them through normal page-cache or delayed writeback logic, graphics output will be unreliable.

## 4. `/dev/mem` And MMIO Mapping

For many old graphics drivers, `/dev/mem` is not optional.

Your kernel should support:

- opening `/dev/mem`
- seeking within it
- mapping it with `mmap()`
- direct page-fault resolution for physical MMIO/VRAM pages

Design rules that help:

- map the requested physical page directly into userspace
- mark MMIO mappings with the right cache attributes for your architecture
- do not copy VRAM into anonymous RAM pages
- do not delay stores waiting for `msync()` or unmap time

If you want to boot old X without a native DRM/KMS stack, `/dev/mem` compatibility is usually part of the minimum viable path.

## 5. x86 Legacy Compatibility: `vm86`, VBE, And Port I/O

Older x86 X servers often still rely on BIOS-era assumptions.

You may need:

- `vm86` or a compatibility shim that emulates only the VBE calls you care about
- VBE controller-info and mode-info queries
- mode set/get support
- scanline and palette calls
- save/restore and miscellaneous VBE capability calls
- PCI config-space and VGA port access

You also need a story for privileged port I/O:

- `iopl`
- `ioperm`
- or an equivalent mechanism that grants the X process exactly the ports it needs

On x86, losing port-I/O privileges across syscalls, scheduling, or signal return can break the server in ways that look unrelated to graphics. Keep the privilege model stable across all kernel return paths.

## 6. Keyboard And Mouse Devices

Getting a picture on screen is only half of X bring-up. The next step is usable input.

Your kernel should provide:

- keyboard events routed to the active VT
- console keyboard mode control for X probe/setup code
- a mouse device node X already knows how to use, commonly `/dev/input/mice`
- blocking, nonblocking, and pollable input reads
- optional async delivery with `FASYNC`, `F_SETOWN`, and `F_SETSIG`

For mouse compatibility, the most practical approach is usually:

- expose a PS/2-like byte stream
- accept common tty-style/ioctl probe calls without surprising userspace
- support the command/response traffic the driver expects during initialization

Many X mouse paths do a short probe phase first and then switch to long-lived event delivery. Make sure both phases work. A device that only survives probe or only works in blocking reads is not enough.

## 7. PTYs And Terminal Plumbing

Desktop sessions and tools like `xterm` require proper terminal infrastructure.

Your kernel should provide:

- `/dev/ptmx`
- PTY master/slave allocation
- slave terminals that behave like normal ttys
- enough termios support for interactive shells
- controlling-terminal handoff for newly spawned terminal windows

If PTYs are missing or incomplete, X itself may start, but terminal emulators and session startup scripts will quickly fail.

## 8. Local Unix Sockets

An X server is only useful if clients can connect to it.

Your kernel should support:

- `AF_UNIX` stream sockets
- pathname bind/listen/accept
- correct connect semantics for `/tmp/.X11-unix/X0`
- socket file metadata compatible enough for `stat` and `fstat`

Semantics matter more than raw API count:

- listeners must become readable when a connection is pending
- connected sockets must block and wake correctly
- nonblocking I/O must return `-EAGAIN` when appropriate
- write readiness should reflect actual send capacity
- closing one side must wake the other side

If local sockets are weak, the typical symptom is that the X server appears to start, but `xinit`, `xterm`, or session helpers still cannot connect or make progress.

## 9. `select`, `poll`, `readv`, And `writev`

The X server and its helper processes are event-loop heavy.

You should implement:

- `select(2)`
- `poll(2)`
- the Linux i386 `__NR__newselect` calling convention if targeting old userspace
- `readv(2)`
- `writev(2)`

The important parts are behavioral:

- wait registration must not lose wakeups
- readiness must be recomputed after registration
- stale waiters must be removed after wakeup
- vectored I/O should behave like one logical I/O operation, not many unrelated per-buffer reads or writes

This area often determines whether late startup succeeds once the visible display is already working.

## 10. Signals And Timers

Old X servers, toolkits, and session helpers are very sensitive to signal ABI details.

Your kernel should support:

- `sigaction`
- `rt_sigaction`
- `sigreturn`
- `rt_sigreturn`
- signal masks and pending-signal logic
- `SA_SIGINFO`
- `SA_RESTORER`
- altstack support
- `alarm`, `setitimer`, `getitimer`

On i386 Linux-style userspace, signal compatibility means more than invoking a handler. You need to preserve and restore:

- general registers
- segment registers
- stack pointer
- return instruction pointer
- flags
- signal mask state

You also need correct interruption semantics for code waiting in:

- `select`
- `poll`
- `read`
- socket waits
- sleep/timer calls

If your signal frame or return path is slightly wrong, helper processes often fail before they ever reach application-level logic.

## 11. SysV IPC And Shared Memory

If you are targeting older Linux desktop userspace, implement System V shared memory early.

At minimum, support:

- `shmget`
- `shmat`
- `shmdt`
- `shmctl`

The shared-memory model should be coherent across:

- multiple attachments
- forked processes
- shared mappings with consistent backing

Some old graphics and compatibility paths still assume SysV SHM exists even when the visible rendering path mostly uses the framebuffer directly.

## 12. `mmap()` And VM Semantics

X bring-up exercises VM behavior harder than a shell workload does.

Your kernel should support:

- `MAP_FIXED`
- shared vs private mappings
- copy-on-write for ordinary private writable pages
- character-device mappings
- large file-backed and MMIO mappings
- correct unmap behavior for partial overlaps

A few design distinctions are especially important:

- RAM-backed pages participate in COW and normal reference counting
- MMIO mappings do not
- dirty writeback rules that make sense for regular files do not apply to `/dev/mem`

Also validate `mmap()` targets carefully. Rejecting unsupported objects early is much better than letting userspace crash later through a nonsense mapping.

## 13. Process, Session, And Scheduler Compatibility

Graphical userspace expects more process API surface than a single-user shell does.

Your kernel should have solid behavior for:

- `fork`
- `execve`
- `waitpid`
- `setsid`
- `setpgid`
- session leaders and controlling terminals
- sleeping and wakeups used by event loops

If you are targeting glibc and older desktop stacks, it also helps to implement the common scheduler-query calls, even if your scheduler policy model is simple:

- `sched_getparam`
- `sched_getscheduler`
- `sched_setparam`
- `sched_setscheduler`
- `sched_get_priority_max`
- `sched_get_priority_min`
- `sched_rr_get_interval`

Often these functions can return conservative Linux-like answers and still satisfy userspace.

## 14. Filesystem And Namespace Expectations

The graphical stack expects a familiar Linux-style namespace.

Make sure you have:

- `/tmp`
- `/tmp/.X11-unix`
- `/dev/tty`
- `/dev/console`
- VT device nodes
- `/dev/ptmx`
- `/dev/pts/*`
- `/dev/mem`
- `/dev/input/mice`

It also helps if virtual device nodes behave sanely under:

- `open`
- `stat`
- `fstat64`
- `chmod`
- `chown`

The goal is not to emulate every inode detail perfectly. The goal is to avoid surprising userspace in the common setup paths it already assumes.

## 15. User-Return Hygiene

Old userspace is sensitive to low-level CPU state.

On every return to ring 3, make sure you are deliberately handling:

- user-visible flags
- privilege bits
- segment selector restoration
- pending signal delivery
- fault handling for illegal privileged accesses

Avoid leaking stale control bits or partially restored state from kernel execution into userspace. Problems here often show up as strange crashes in helper binaries rather than in the X server itself.

## 16. Recommended Bring-Up Order

If you are implementing this from scratch, a practical order is:

1. Get stable ELF execution, signals, and ordinary shell userspace working.
2. Add VT control, controlling-terminal support, and PTYs.
3. Implement `AF_UNIX`, `select`, `poll`, `readv`, and `writev`.
4. Add `/dev/mem` and MMIO-aware `mmap()`.
5. Add VBE/legacy x86 compatibility and port-I/O permissions.
6. Add `/dev/input/mice` compatibility and runtime input delivery.
7. Add SysV shared memory.
8. Only then start chasing higher-level desktop startup issues.

This order reduces the chance that you spend time debugging "graphics problems" that are really caused by missing IPC, signal, or terminal features.

## 17. Minimum Checklist

Before you seriously debug X startup, your kernel should already have:

- VT ioctls and `KD_TEXT`/`KD_GRAPHICS`
- controlling-terminal and session semantics
- PTYs and `/dev/ptmx`
- `/dev/mem` with `mmap()` support
- MMIO-aware page-fault handling
- some VBE or card compatibility path the X driver can use
- stable port-I/O permission support
- `/dev/input/mice`
- `AF_UNIX` stream sockets
- correct `select` and `poll`
- `readv` and `writev`
- Linux-compatible enough signal and timer behavior
- SysV shared memory
- `MAP_FIXED` and correct shared/private mapping semantics

Once all of that exists, X bring-up becomes a tractable integration task instead of a blind search through unrelated kernel subsystems.
