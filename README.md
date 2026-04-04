# MOS Kernel

32-bit x86 (i686) educational OS kernel targeting Linux 2.4.20-8 (Red Hat 9) userspace binary compatibility. Runs in QEMU via GRUB/Multiboot.

With some features we are never going to implement (like kernel module), the booting process will show lots of errors, but it can $\color{green}{\textbf{successfully}}$ boot the Red Hat 9 system!

> [!NOTE]
> The `root` user has no password. Enjoy.

<table>
  <tr>
    <td><img src="doc/screenshot/boot.png"></td>
    <td><img src="doc/screenshot/login_prompt.png"></td>
  </tr>
</table>

**Build:** `make` / `make rebuild` / `make run`
> [!NOTE]
> The first time rum `make` will setup the system image for you.

---

## Documentation

### Architecture
| Section                                 | Summary                                                                                                                       |
| --------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| [Architecture Overview](doc/overall.md) | Full architecture overview: memory layout, boot sequence, all subsystems, key constants                                       |
| [Boot Stage 1](doc/boot_stage1.md)      | GDT/IDT setup, PIC init, initial 8 MB page tables, paging enable, EIP/ESP transition, physical memory allocator               |
| [Boot Stage 2](doc/boot_stage2.md)      | Subsystem init order, `KERNEL_INIT` table, SMP startup, first userspace process                                               |
| [Interrupt Handling](doc/interrupts.md) | IDT setup, entry stubs, stack layout at every stage, dispatcher, syscall/page-fault/IRQ/IPI paths, IF enable/disable timeline |
| [Physical Memory](doc/mm_physical.md)   | Buddy allocator, page descriptors, reference counting, CoW, dirty tracking                                                    |
| [Virtual Memory](doc/mm_virtual.md)     | Page table management, VM region map, mmap/munmap, demand paging, CoW, page cache                                             |
| [Process & Scheduler](doc/ps.md)        | MPRQ, context switch, fork/exit/waitpid, signals, synchronization primitives                                                  |
| [Virtual File System](doc/vfs.md)       | Inode/file object model, mount tree, fs type registry, fd API, ext4 backend, loop device                                      |
| [TTY / PTY](doc/tty.md)                 | Virtual consoles, line discipline (termios/ANSI), PTY pairs, VT switching, bash spawner                                       |
| [Framebuffer / VGA](doc/vga.md)         | fb_drv_t interface, Bochs/VBE driver, VMware SVGA2 driver, font rendering, cell model                                         |
| [Poll / Select](doc/poll.md)            | Four-phase wait loop, lost-wakeup prevention, poll_wait driver interface, socket integration, fallback timer                  |
| [ELF Loader](doc/elf_exec.md)           | Segment mapping, BSS handling, dynamic linker loading, execve lifecycle, initial stack layout                                 |
| [Signal Handling](doc/signals.md)       | Delivery engine, vDSO trampoline, signal frame layout, sigaction/sigprocmask/sigreturn, alarm, process group signals          |
| [Network Stack](doc/network.md)         | e1000 NIC driver, lwIP integration, socket layer, TCP/UDP/RAW operations, ioctl, blocking model                               |
| [Kernel Heap Allocator](doc/malloc.md)  | Segregated free-lists, block layout, coalescing, heap extension, user-space sys_brk                                           |
| [Locking Primitives](doc/locks.md)      | Spinlock, condition variable, mutex, readers-writer lock, semaphore                                                           |
| [ATA/IDE Disk Driver](doc/hdd.md)       | Bus Master DMA + PIO modes, PRDT layout, IRQ sync, LRU write-back block cache, partition discovery                            |

### Reference

| Section                                  | Summary                                                                         |
| ---------------------------------------- | ------------------------------------------------------------------------------- |
| [Build Guide](doc/build.md)              | Build dependencies, compiler setup, run modes, debugging, profiling, disk image |
| [Profiling](doc/profiling.md)            | Flat EIP histogram and flamegraph profiler, reading flamegraphs, example analysis |
| [Disk Image](doc/disk_image.md)          | Mount `rh9.qcow2` via qemu-nbd, copy binaries into it, recreate from scratch    |
| [Todo](doc/todo.md)                      | Feature checklist                                                               |
| [Screenshots](doc/screenshots.md)        | Some screen shots about mos running                                             |
| [SysV Init Boot Journal](doc/systemv.md) | Six bugs fixed to boot RH9 userspace to a login prompt                          |
