# Bringing Up X On Your Own Kernel

## X11 Architecture First

Before diving into kernel compatibility details, it helps to frame what the X11 stack actually is.

X11 is not a single graphics program. It is a distributed windowing system with a split responsibility model:

- the kernel provides CPU scheduling, memory management, device access, interrupts, files, sockets, ttys, and process control
- the X server owns the display hardware, input devices, screen state, and the core X11 protocol endpoint
- window managers and desktop/session components sit on top of the X server and decide policy such as focus, decoration, placement, menus, and session startup
- X clients such as `xterm`, `twm`, `xclock`, or GNOME programs speak the X11 protocol to the server over a Unix socket

That means an X desktop is really a cooperation pipeline, not a direct "app -> GPU" model.

### High-Level Layering

```text
+---------------------------------------------------------------+
| X clients: xterm, xclock, xkbcomp, panel, WM, desktop apps   |
| use Xlib/XCB/toolkits and speak X11 protocol                  |
+--------------------------+------------------------------------+
                           |
                           | AF_UNIX socket: /tmp/.X11-unix/X0
                           v
+---------------------------------------------------------------+
| X server: XFree86 / early Xorg                               |
| - accepts X11 protocol requests                              |
| - tracks windows, pixmaps, GC, cursor, colormap              |
| - probes video/input drivers                                 |
| - owns the active VT / graphics console                      |
| - translates kernel devices into X events                    |
+-----------+----------------------+----------------------------+
            |                      |
            | ioctls, mmap, iopl   | read/poll/fasync
            | vm86/VBE, /dev/mem   | /dev/input/mice, tty ioctls
            v                      v
+---------------------------------------------------------------+
| MOS kernel                                                    |
| - scheduler, signals, fork/exec, timers                      |
| - VM, mmap, page faults, /dev/mem MMIO mapping               |
| - tty/VT/session control, PTYs                               |
| - AF_UNIX sockets, select/poll/readv/writev                  |
| - PCI/port I/O privileges, vm86/VBE shim, IRQ delivery       |
+-----------+----------------------+----------------------------+
            |                      |
            v                      v
+-------------------------+   +--------------------------------+
| video hw / framebuffer  |   | keyboard / PS/2 mouse / timer |
| PCI BAR, VBE, VRAM      |   | IRQs and event sources         |
+-------------------------+   +--------------------------------+
```

### Who Does What

The most important design boundary is this:

- the kernel is responsible for mechanism
- X and the desktop stack are responsible for graphics and UI policy

In practice, the kernel does not know what a top-level window, decoration, menu, or clipboard manager is. The kernel only knows how to:

- schedule X and its clients
- let the X server become the controlling process for a VT
- deliver keyboard and mouse bytes
- map the framebuffer or MMIO registers
- let processes talk over local sockets
- preserve Linux-like syscall and signal ABI behavior

Then the X server builds higher-level graphics behavior on top:

- creates the root window
- keeps window trees and clipping state
- accepts draw requests from clients
- composites or copies pixels into framebuffer-visible memory
- converts input bytes into X key and pointer events
- sends those events to the focused client

And then a window manager adds policy:

- reparents or decorates windows
- chooses focus rules
- responds to move/resize requests
- manages stacking order

So when people say "bring up X", what they usually mean is:

1. the kernel is Linux-compatible enough for the X server to run
2. the X server can own the screen and publish `DISPLAY=:0`
3. clients can connect and create windows
4. a window manager can make the session usable

### How X Co-Works With The Kernel

An old XFree86-style stack depends on the kernel in several separate directions at once.

#### 1. Process, signal, and timer substrate

The server, `xinit`, helper tools, and clients all rely on normal Unix process semantics:

- `fork`, `execve`, `waitpid`
- `sigaction`, `rt_sigaction`, `sigreturn`, `rt_sigreturn`
- `alarm`, `setitimer`, `nanosleep`
- scheduler state surviving preemption and syscall return

In MOS, this matters directly because X startup exercises signal and timer paths heavily, and user-return state is restored in [src/int/int.c](/home/zhengjia/project/mos/src/int/int.c:64).

#### 2. VT and tty ownership

The X server usually starts from a text login and must take control of a real console:

- allocate or choose a VT
- become controlling process for that terminal
- switch that VT to graphics mode
- stop the text console renderer from painting over it

Without this, the screen may flicker, be overwritten by tty redraws, or fail before graphics setup.

#### 3. Graphics/MMIO path

Old XFree86 video paths often work by discovering a mode through BIOS/VBE-style interfaces, then directly mapping VRAM:

- VBE or card probe figures out mode and framebuffer location
- `/dev/mem` plus `mmap()` exposes the physical BAR / linear framebuffer
- userspace stores hit MMIO or VRAM directly

MOS handles that in the page-fault path by mapping `/dev/mem` requests as direct I/O pages instead of ordinary cached file pages in [src/mm/pagefault.c](/home/zhengjia/project/mos/src/mm/pagefault.c:118).

#### 3a. How the final image reaches the screen

The last step is easy to miss because X itself mostly thinks in terms of draw requests and framebuffer memory, not "photons on a monitor."

Conceptually, the visible image appears in these stages:

1. a client asks X to draw something
2. the X server updates its internal screen/window/pixmap state
3. the X server writes pixels into the mapped framebuffer or card-visible memory
4. the active display path scans that memory and turns it into the monitor image

For an old framebuffer-style stack, the server often renders "directly into the thing being scanned out." There is no modern DRM/KMS compositor in between. The server's writes land in VRAM, and the display hardware keeps reading that VRAM repeatedly to refresh the screen.

In other words:

- X decides what pixels should exist
- the kernel makes the framebuffer mapping legal and stable
- the video hardware continuously reads the resulting pixel buffer
- the monitor shows whatever the current scanout buffer contains

In MOS there is one more practical detail: the graphics VT must remain the active visible target, and the kernel-side presentation path must keep pushing the graphics framebuffer to the display instead of letting tty text rendering reclaim the screen. The GUI journal explicitly notes that periodic graphics presentation was needed so framebuffer writes on graphics VTs became visible during X runtime.

```text
client draw request
   -> X11 protocol message
   -> X server updates draw state
   -> X driver writes pixels into mapped VRAM/framebuffer
   -> display engine scans framebuffer memory
   -> DAC/display output presents pixels
   -> monitor shows the final image
```

This is also why several different bugs can produce "X seems alive but nothing appears":

- the server may be drawing into the wrong address
- `/dev/mem` mappings may succeed but not actually hit MMIO/VRAM correctly
- the wrong VT may still be active
- tty redraw may overwrite the graphics VT
- the present/scanout path may not be refreshing the visible display
- the cursor may appear even while most window pixels are not being updated yet

#### 4. x86 compatibility path

For this codebase, old X startup is not only "draw pixels"; it also touches x86 legacy behavior:

- `vm86`-style BIOS/VBE compatibility
- PCI probing
- `iopl` / port-I/O permissions
- VMware SVGA/VBE mode setup assumptions

MOS carries explicit VBE/vm86 compatibility shims for that path in [src/syscall/syscall_vm86.c](/home/zhengjia/project/mos/src/syscall/syscall_vm86.c:1).

#### 5. Input path

The X server does not talk to hardware interrupts directly. The kernel handles IRQs and exposes device streams. X then consumes those streams from userland:

- keyboard input arrives through tty/console-compatible paths
- mouse bytes arrive through `/dev/input/mice`
- poll/read/async signal behavior wakes the server
- X translates that into motion/button/key events for clients

#### 6. Client/server IPC path

Once the server is alive, nearly all app interaction becomes protocol traffic:

- X listens on `/tmp/.X11-unix/X0`
- `xinit` and clients connect over `AF_UNIX`
- requests, replies, events, and errors flow through the socket
- helper processes and terminal emulators also rely on PTYs, `select`, `poll`, `readv`, and `writev`

This is why a kernel can have a visible screen but still not have a usable X desktop: the graphics path may work while the local-socket or event-loop path is still broken.

### Architecture Graph For A Typical `startx`

```text
login shell
   |
   v
startx
   |
   v
xinit
   |------------------------ forks -------------------------|
   |                                                        |
   v                                                        v
X server (:0)                                        client/session script
   |                                                        |
   | open VT, KDSETMODE, VBE, /dev/mem, input              | waits for server
   | create /tmp/.X11-unix/X0                              | then connects to :0
   v                                                        v
kernel services <------------------------------------ AF_UNIX connect
   |
   +--> tty/VT/session control
   +--> vm86/VBE and port I/O privilege path
   +--> page faults for framebuffer mmap
   +--> mouse/keyboard delivery
   +--> scheduler, signals, timers
   +--> AF_UNIX socket transport
```

### Startup Call Sequence

The full code path is large, but conceptually a successful X startup looks like this:

```text
startx
  -> xinit
     -> fork()
     -> child: execve(X server)
     -> parent: wait for server readiness

X server
  -> open tty / VT
  -> ioctl(VT_OPENQRY / VT_ACTIVATE / VT_WAITACTIVE / KDSETMODE)
  -> probe graphics path
  -> vm86/VBE queries or card probe
  -> open("/dev/mem")
  -> mmap(framebuffer/MMIO)
  -> open input devices
  -> create/bind/listen("/tmp/.X11-unix/X0")
  -> initialize root window and screen resources
  -> enter main event loop

xinit parent
  -> connect("/tmp/.X11-unix/X0")
  -> exec session clients / window manager

client
  -> socket(AF_UNIX)
  -> connect(X0)
  -> send X11 setup/auth request
  -> create window / GC / pixmaps
  -> server draws and sends Expose/input events
```

### Runtime Event Sequence

After startup, a normal interaction has two independent flows: rendering requests from clients to the server, and hardware input from the kernel into the server and then back out to clients as X events.

```text
Rendering path:
client -> X11 request -> X server -> framebuffer/MMIO writes
       -> scanout/present path -> visible screen

Input path:
mouse/keyboard IRQ -> kernel driver -> device file readiness
                    -> X server reads bytes
                    -> X server chooses target window
                    -> X event delivered to client
```

That split is why the kernel never directly sends a "mouse clicked window 5" message. The kernel only delivers low-level device readiness and bytes. The X server interprets them in the context of window focus, grabs, keymaps, and pointer position.

You can think of the visible screen as the result of a loop that never stops while the mode is active:

```text
X updates framebuffer contents
        ||
        \/ 
display hardware repeatedly scans current framebuffer memory
        ||
        \/
monitor refresh shows newest pixels
```

So the "final image" is not usually produced by a one-shot `show()` call. It appears because the X server mutates the buffer that the hardware is already scanning for display.

### Why This Matters For Bring-Up

This architecture explains why old X bring-up failures can look unrelated:

- a bad signal return path can kill the server after the screen already appears
- weak `AF_UNIX` readiness can make `xinit` fail even though `X` created the socket
- incorrect `/dev/mem` mapping semantics can corrupt framebuffer access
- missing VT semantics can make the text console overwrite graphics
- broken PTYs can let X start but make `xterm` unusable

So the right mental model is not "make graphics work". It is "make enough Unix, tty, VM, IPC, and x86 compatibility contracts work together that the X server can act as the user-space owner of the display."

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
