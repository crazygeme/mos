# MOS Kernel

MOS is an educational monolithic kernel for 32-bit x86 (i686) and 64-bit
x86 (AMD64), with Linux syscall and ELF binary compatibility. It boots through
GRUB/Multiboot and runs in QEMU, with SMP and optional KVM acceleration.

MOS runs modern GNU userspace through [GNU/MOS](https://github.com/crazygeme/gnu-mos),
which builds x86 and x64 systems with glibc 2.42, GNU utilities, SysV init, and
an Xfce desktop. The x64 kernel also runs i386 binaries in compatibility mode.
Linux compatibility is incomplete; application support depends on the kernel
interfaces each program requires.

The bundled `run.sh` workflow uses a separate Red Hat 9 image with GNOME.

## Screenshots

The screenshots show the x64 GNU/MOS system launched with `./lfs run`, using
two virtual CPUs, 8192 MiB RAM, and a VirGL-capable VirtIO GPU. Click any image
to view the original capture, or browse the [full gallery](doc/screenshots.md).

### Xfce Desktop

<p align="center">
  <a href="doc/screenshot/gui1.png">
    <img src="doc/screenshot/framed/gui1.svg" alt="GNU/MOS Xfce desktop" width="800">
  </a>
</p>

<details>
<summary>More screenshots: applications, boot and login</summary>

### Desktop Applications

<p align="center">
  <a href="doc/screenshot/gui2.png">
    <img src="doc/screenshot/framed/gui2.svg" alt="GNU/MOS desktop applications" width="800">
  </a>
</p>

### Visual Studio Code

<p align="center">
  <a href="doc/screenshot/vscode.png">
    <img src="doc/screenshot/framed/vscode.svg" alt="Visual Studio Code on GNU/MOS" width="800">
  </a>
</p>

### Microsoft Edge

<p align="center">
  <a href="doc/screenshot/browser.png">
    <img src="doc/screenshot/framed/browser.svg" alt="Microsoft Edge displaying Baidu" width="800">
  </a>
</p>

### Boot

<p align="center">
  <a href="doc/screenshot/boot.png">
    <img src="doc/screenshot/framed/boot.svg" alt="Kernel and SysV init startup" width="800">
  </a>
</p>

### XDM Login

<p align="center">
  <a href="doc/screenshot/login_prompt.png">
    <img src="doc/screenshot/framed/login_prompt.svg" alt="GNU/MOS XDM login prompt" width="800">
  </a>
</p>

</details>

> [!NOTE]
> Linux compatibility is incomplete. The GNU/MOS launchers for Visual Studio
> Code and Microsoft Edge supply `--no-sandbox` because Chromium namespace
> and seccomp isolation are unavailable.

---

## Quick Start

### Prerequisites

**Ubuntu / Debian**

```sh
sudo apt install build-essential gcc-multilib qemu-system-x86 qemu-utils python3 dnsmasq unzip iproute2 iptables
```

**macOS**

```sh
brew install qemu python
# Install i686-elf-* for x86 or x86_64-elf-* for x64, including GCC and binutils.
```

### Build & Run

```sh
make ARCH=x86                 # x86 release -> out/x86/release/
make ARCH=x64                 # AMD64 release -> out/x64/release/
make ARCH=x64 BUILD=debug      # AMD64 debug -> out/x64/debug/
./run.sh                      # build and boot AMD64 release with Red Hat 9
./run.sh arch=x86              # build and boot x86 release with Red Hat 9
./run.sh text                  # boot through init to a text login (runlevel 3)
./run.sh debug                 # boot AMD64 debug, paused for GDB on port 8888
./run.sh test kvm              # build and boot AMD64 tests with KVM
```

`make` defaults to x86; `run.sh` defaults to x64, two CPUs, and 8192 MiB RAM.
Select a launch configuration with `arch=x86|x64`, `smp=N`, and `ram=N`.
The script uses `rh9.qcow2`, extracts `redhat9.img.zip` if needed, and stages
files from `tools/guest/` before booting. Linux launches require `sudo` for
disk setup and, outside test mode, TAP networking. The RH9 root password is
`123456`. On Linux hosts with KVM support, add `kvm` for hardware acceleration.

For a modern GNU system, use the build and image instructions in
[GNU/MOS](https://github.com/crazygeme/gnu-mos). Its launcher manages separate
x86 and x64 userspace images.

See [Build Guide](doc/build.md) for debugging, profiling, TAP networking, and disk image management.

---

## Documentation

### Architecture

| Document | Summary |
| --- | --- |
| [Architecture Overview](doc/overall.md) | x86 and AMD64 backends, memory layout, boot sequence, subsystems, key constants |
| [Boot Stage 1](doc/boot_stage1.md) | GDT/IDT setup, PIC init, initial page tables, paging enable, EIP/ESP transition, physical memory allocator |
| [Boot Stage 2](doc/boot_stage2.md) | Subsystem init order, `KERNEL_INIT` table, SMP startup, first userspace process |
| [Interrupt Handling](doc/interrupts.md) | IDT setup, entry stubs, stack layout, dispatcher, syscall/page-fault/IRQ/IPI paths, IF timeline |
| [Fast System Calls](doc/fast_syscall.md) | i386 SYSENTER/vDSO selection, AMD64 SYSRETQ checks, compatibility returns, and isolated QEMU validation |
| [Physical Memory](doc/mm_physical.md) | Buddy allocator, page descriptors, reference counting, CoW, dirty tracking |
| [Virtual Memory](doc/mm_virtual.md) | Page table management, VM region map, mmap/munmap, demand paging, CoW, page cache |
| [Process & Scheduler](doc/ps.md) | MPRQ, context switch, fork/exit/waitpid, signals, synchronization primitives |
| [Virtual File System](doc/vfs.md) | Inode/file object model, mount tree, fs type registry, fd API, ext4 backend, loop device |
| [TTY / PTY](doc/tty.md) | Virtual consoles, line discipline (termios/ANSI), PTY pairs, VT switching, userspace login terminals |
| [Devices and Drivers](doc/devices.md) | PCI discovery, driver matching, initialization, and `/dev` / `/sys` ownership |
| [Framebuffer / VGA](doc/vga.md) | fb_drv_t interface, Bochs/VBE driver, VMware SVGA2 driver, font rendering, cell model |
| [X Bring-Up Requirements](doc/x_bringup.md) | Practical checklist for building the kernel features needed to boot an old Linux/XFree86-style graphical desktop |
| [Epoll](doc/epoll.md) | Persistent readiness subscriptions, level and edge delivery, one-shot rearming, Linux ABI, and performance validation |
| [Poll / Select](doc/poll.md) | Four-phase wait loop, lost-wakeup prevention, poll_wait driver interface, socket integration |
| [ELF Loader](doc/elf_exec.md) | Segment mapping, BSS handling, dynamic linker loading, execve lifecycle, initial stack layout |
| [Signal Handling](doc/signals.md) | Delivery engine, inline trampoline, signal frame layout, sigaction/sigprocmask/sigreturn, altstack, sigsuspend |
| [Network Stack](doc/network.md) | e1000 NIC driver, lwIP integration, socket layer, TCP/UDP/RAW operations, ioctl, blocking model |
| [Kernel Heap Allocator](doc/malloc.md) | Segregated free-lists, block layout, coalescing, heap extension, user-space sys_brk |
| [Locking Primitives](doc/locks.md) | Spinlock, condition variable, mutex, readers-writer lock, semaphore |
| [ATA/IDE Disk Driver](doc/hdd.md) | Bus Master DMA + PIO modes, PRDT layout, IRQ sync, LRU write-back block cache, partition discovery |

### Reference

| Document | Summary |
| --- | --- |
| [Testing](doc/testing.md) | Kernel-mode KTEST framework, shell script tests, /proc/tests/ interface |
| [Build Guide](doc/build.md) | Build dependencies, compiler setup, run modes, debugging, profiling, disk image |
| [Profiling](doc/profiling.md) | Sampling profilers, flamegraphs, and process-launch measurements |
| [Disk Image](doc/disk_image.md) | Mount `rh9.qcow2` via qemu-nbd, copy binaries into it, recreate from scratch |
| [SysV Init Boot Journal](doc/systemv.md) | Six bugs fixed to boot RH9 userspace to a login prompt |
| [NPTL Journal](doc/nptl_journal.md) | `clone()`/NPTL debugging notes, `CLONE_CHILD_SETTID` fixes, and `posix_signal` race analysis |
| [Performance Journal](doc/perf_journal.md) | Dated performance measurements, profiling findings, and optimization records |
| [Bug Fix Journal](doc/bugfix_journal.md) | Dated fixes, ABI and concurrency records, root causes, and validation |
| [GUI Journal](doc/gui_journal.md) | XFree86 / `startx` debugging progress, compatibility fixes, and current GUI status |
| [Screenshots](doc/screenshots.md) | Screenshots of MOS running, including the GUI desktop |
| [Todo](doc/todo.md) | Feature checklist |
