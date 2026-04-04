# MOS Kernel

> A 32-bit x86 (i686) educational OS kernel with Linux 2.4.20-8 (Red Hat 9) userspace binary compatibility, running in QEMU via GRUB/Multiboot.

Some kernel modules are intentionally unimplemented, so the boot log will show errors — but MOS **successfully boots to a Red Hat 9 login prompt**.


| Boot                             | Login Prompt                              |
| -------------------------------- | ----------------------------------------- |
| ![Boot](doc/screenshot/boot.png) | ![Login](doc/screenshot/login_prompt.png) |

---

## Quick Start

### Prerequisites

**Ubuntu / Debian**
```sh
sudo apt install build-essential gcc-multilib qemu-system-x86 python3
```

**macOS**
```sh
brew install qemu python
brew tap nativeos/i386-elf-toolchain && brew install i386-elf-gcc i386-elf-binutils
```

### Build & Run

```sh
make -j$(nproc)   # build kernel (sets up disk image on first run)
./run.sh          # boot into Red Hat 9 (SysV init → login prompt)
./run.sh bash     # boot directly into bash
```

> [!NOTE]
> The `root` user has no password.

> [!TIP]
> On Linux with KVM support, add `kvm` for **10×** faster emulation:
> ```sh
> ./run.sh kvm
> ./run.sh bash kvm
> ```

See [Build Guide](doc/build.md) for debugging, profiling, TAP networking, and disk image management.

---

## Documentation

### Architecture

| Document                                | Summary                                                                                                         |
| --------------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| [Architecture Overview](doc/overall.md) | Memory layout, boot sequence, all subsystems, key constants                                                     |
| [Boot Stage 1](doc/boot_stage1.md)      | GDT/IDT setup, PIC init, initial 8 MB page tables, paging enable, EIP/ESP transition, physical memory allocator |
| [Boot Stage 2](doc/boot_stage2.md)      | Subsystem init order, `KERNEL_INIT` table, SMP startup, first userspace process                                 |
| [Interrupt Handling](doc/interrupts.md) | IDT setup, entry stubs, stack layout, dispatcher, syscall/page-fault/IRQ/IPI paths, IF timeline                 |
| [Physical Memory](doc/mm_physical.md)   | Buddy allocator, page descriptors, reference counting, CoW, dirty tracking                                      |
| [Virtual Memory](doc/mm_virtual.md)     | Page table management, VM region map, mmap/munmap, demand paging, CoW, page cache                               |
| [Process & Scheduler](doc/ps.md)        | MPRQ, context switch, fork/exit/waitpid, signals, synchronization primitives                                    |
| [Virtual File System](doc/vfs.md)       | Inode/file object model, mount tree, fs type registry, fd API, ext4 backend, loop device                        |
| [TTY / PTY](doc/tty.md)                 | Virtual consoles, line discipline (termios/ANSI), PTY pairs, VT switching, bash spawner                         |
| [Framebuffer / VGA](doc/vga.md)         | fb_drv_t interface, Bochs/VBE driver, VMware SVGA2 driver, font rendering, cell model                           |
| [Poll / Select](doc/poll.md)            | Four-phase wait loop, lost-wakeup prevention, poll_wait driver interface, socket integration                    |
| [ELF Loader](doc/elf_exec.md)           | Segment mapping, BSS handling, dynamic linker loading, execve lifecycle, initial stack layout                   |
| [Signal Handling](doc/signals.md)       | Delivery engine, vDSO trampoline, signal frame layout, sigaction/sigprocmask/sigreturn, alarm                   |
| [Network Stack](doc/network.md)         | e1000 NIC driver, lwIP integration, socket layer, TCP/UDP/RAW operations, ioctl, blocking model                 |
| [Kernel Heap Allocator](doc/malloc.md)  | Segregated free-lists, block layout, coalescing, heap extension, user-space sys_brk                             |
| [Locking Primitives](doc/locks.md)      | Spinlock, condition variable, mutex, readers-writer lock, semaphore                                             |
| [ATA/IDE Disk Driver](doc/hdd.md)       | Bus Master DMA + PIO modes, PRDT layout, IRQ sync, LRU write-back block cache, partition discovery              |

### Reference

| Document                                 | Summary                                                                           |
| ---------------------------------------- | --------------------------------------------------------------------------------- |
| [Build Guide](doc/build.md)              | Build dependencies, compiler setup, run modes, debugging, profiling, disk image   |
| [Profiling](doc/profiling.md)            | Flat EIP histogram and flamegraph profiler, reading flamegraphs, example analysis |
| [Disk Image](doc/disk_image.md)          | Mount `rh9.qcow2` via qemu-nbd, copy binaries into it, recreate from scratch      |
| [SysV Init Boot Journal](doc/systemv.md) | Six bugs fixed to boot RH9 userspace to a login prompt                            |
| [Screenshots](doc/screenshots.md)        | Screenshots of MOS running                                                        |
| [Todo](doc/todo.md)                      | Feature checklist                                                                 |
