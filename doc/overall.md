# MOS Kernel — Architecture Overview

## 1. Overview

MOS is a 32-bit x86 (i686) monolithic kernel targeting Linux 2.4.20-8 (Red Hat 9)
userspace binary compatibility. It runs in QEMU and boots via GRUB/Multiboot.

- **Architecture:** i686, 32-bit protected mode, single address space
- **Compatibility target:** Linux 2.4.20-8 (RH9), ELF32 dynamic binaries
- **Build:** `make` / `make rebuild` / `make run`
- **Compiler:** gcc -m32 -march=i686

---

## 2. Memory Layout

```
0x00000000 - 0x00007FFF   Reserved / BIOS
0x00008000                AP trampoline (SMP startup code)
0x00009000                AP params page
0x00100000                Kernel image (physical load address)

Virtual address space (after paging enabled):
0x00000000 - 0x3FFFFFFF   User space (TASK_UNMAPPED_BASE = 0x40000000)
0x40000000 - 0xBFFFEFFF   mmap zone  [TASK_UNMAPPED_BASE, USER_ZONE_END)
0xBFFFF000 - 0xBFFFFFFF   User stack guard + stack (128 pages)
0xC0000000                KERNEL_OFFSET — kernel virtual base
0xC0200000                Kernel image (virtual, mapped from phys 0x100000)
0xC0700000 - 0xC0BFEFFF   Kernel heap (KHEAP_BEGIN / KHEAP_END)
0xC0C00000 - 0xCCBFFFFF   Page table cache (PAGE_TABLE_CACHE_PAGES × 4 KB, default 4096 entries = 16 MB)
0xFEE00000                LAPIC MMIO
0xFEC00000                IOAPIC MMIO
```

The first 8 MB of physical memory is identity-mapped during boot and unmapped
from user space once the scheduler starts (`mm_del_user_map`).

---

## 3. Boot Sequence

```
GRUB/Multiboot
  └─ entry.S          — sets up a temporary stack, calls stage1
       └─ stage1.c    — sets up GDT, enables paging, jumps to virtual address
            └─ stage2.c / kmain_startup()
                 ├─ fb_init / tty_init / klib_init
                 ├─ ps_init          — process subsystem
                 ├─ dsr_init         — deferred service routines
                 ├─ int_enable_all   — enable interrupts (IDT, PIC)
                 ├─ serial / keyboard / time_init / PIT calibration
                 ├─ pf_init          — page fault handler
                 ├─ ACPI parse → LAPIC / IOAPIC / SMP AP startup
                 ├─ ps_create: idle, kmain_process, timer_process
                 └─ ps_kickoff()     — start scheduling
                      └─ kmain_process runs KERNEL_INIT table (ordered 0–8):
                           0: klog_init
                           1: syslog_init
                           2: hdd_init, mount_syscall_init
                           3: fs_mount_root  (ext4 on /dev/hda)
                           4: tty_fs_init
                           5: pts_dev_init, procfs_init
                           6: devfs_init, nic_scan_all
                           7: net_init, syscall_init
                           8: kinit_userspace  → exec /sbin/init
```

### GDT selectors

| Selector | Description                         |
| -------- | ----------------------------------- |
| 0x00     | Null                                |
| 0x08     | Kernel data                         |
| 0x10     | Kernel code                         |
| 0x1B     | User data (RPL3)                    |
| 0x23     | User code (RPL3)                    |
| 0x28+    | TSS per CPU (one per CPU, stride 8) |

---

## 4. Process Management (`src/ps/`)

### Scheduler: Multilevel Priority Ready Queue (MPRQ)

- **Data structures:** one linked list per priority level; each list is
  iterated round-robin. Red-black trees (`lib/rbtree`) index all tasks by PID
  for O(log n) lookup.
- **Time slice:** `DEFAULT_TASK_TIME_SLICE` ticks; decremented in the PIT ISR
  via `force_switch()`.
- **Priorities:** `ps_idle < ps_normal`; kernel and user tasks share the same
  queue structure.
- **Context switch:** `_task_sched()` in `ps_sched.c` — saves/restores
  registers via `intr_frame`, switches CR3 (page directory), reloads TSS ESP0.
- **Wait / sleep:** tasks set `task->timer_due_ms = time_now_ms() + ms` and call
  `task_sched()`; the scheduler skips tasks whose timeout is in the future.

### Process lifecycle

```
ps_create()  →  ps_ready  →  (running)  →  ps_dying  →  reaped by wait()
                               ↕
                           ps_wait (blocked on I/O or sleep)
```

Key structs: `task_struct`, `ps_control`, `tss_struct`.
Locks: `ps_lock` (ready/wait/dying queues), `map_lock` (page tables),
`psid_lock` (PID generation).

### Signals

POSIX signals delivered at interrupt return via `do_signal()` (called from
the PIT ISR). Supports `kill()`, `sigaction()`, `alarm()`.

---

## 5. Memory Management (`src/mm/`)

| File          | Responsibility                                                       |
| ------------- | -------------------------------------------------------------------- |
| `phymm.c`     | Physical page allocator (bitmap, tracks `phymm_begin`–`phymm_end`)   |
| `mm.c`        | Page directory/table management, `kmalloc`/`kfree`, page table cache |
| `mmap.c`      | `mmap`/`munmap`/`brk` — user virtual address space management        |
| `pagefault.c` | `#PF` handler — demand paging, copy-on-write, stack growth           |
| `vdso.c`      | Maps a virtual DSO page (vDSO) into each process                     |

**Page table cache:** a `PAGE_TABLE_CACHE_PAGES`-entry stack (default 4096) pre-allocates page tables from the
reserved region `[PAGE_TABLE_CACHE_BEGIN, PAGE_TABLE_CACHE_END)` to avoid
`kmalloc` during fault handling.

**Copy-on-write:** `fork()` marks shared pages read-only; the first write
triggers `#PF`, which allocates a new physical page and reloads CR3.

---

## 6. Interrupt Handling (`src/int/`)

- **IDT:** 256 entries. Hardware IRQs remapped: IRQ0–IRQ7 → vectors 0x20–0x27,
  IRQ8–IRQ15 → 0x28–0x2F.
- **Syscall:** `int 0x80` (vector 0x80).
- **APIC:** BSP LAPIC initialised in virtual-wire mode (8259A PIC remains
  active for external IRQs). IOAPIC initialised for IPI routing only.
- **IPI vectors:** `IPI_VECTOR_SCHED` (0xF0), `IPI_VECTOR_TLB` (0xF1),
  `IPI_VECTOR_SPURIOUS` (0xFF).
- **DSR (Deferred Service Routines):** lightweight bottom-half mechanism
  (`src/int/dsr.c`); depth-limited queue (`DSR_CACHE_DEPTH = 100`).

---

## 7. Filesystem (`src/fs/`)

### VFS layer

A generic virtual filesystem layer (`vfs.c`) sits above concrete filesystems.
Mount points are stored in a red-black tree keyed by path for O(log n) lookup.

`struct super_block` → `struct inode` → `struct file`

### Concrete filesystems

| FS     | Mount point | Source                           |
| ------ | ----------- | -------------------------------- |
| ext2/4 | `/`         | `src/fs/root.c` + `src/hw/hdd.c` |
| devfs  | `/dev`      | `src/dev/devfs.c`                |
| devpts | `/dev/pts`  | `src/dev/pts.c`                  |
| procfs | `/proc`     | `src/proc/procfs.c`              |
| pipefs | (internal)  | `src/fs/pipe.c`                  |

### Disk cache

`hdd.c` implements a 4096-page write-back LRU block cache (configurable via
`HDD_CACHE_WRITE_POLICY`). Raw disk I/O goes through the Intel 82540EM
emulated IDE controller (`src/hw/hdd.c`).

### Poll / select

`src/fs/poll.c` (`do_poll`) and `src/fs/select.c` (`do_select`) implement
`poll(2)` and `select(2)` using the same four-phase loop:

1. **Check** — query `.poll` on every watched fd; return immediately if any ready.
2. **Register** — call `.poll_wait(fp, task)` on each fd to store the caller's
   task pointer so the driver can wake it.
3. **Re-check** — query `.poll` again to close the lost-wakeup race window.
4. **Sleep** — `time_wait(sleep_ms)`; woken either by a driver calling
   `ps_put_to_ready_queue` or by the deadline expiring.

After waking, `.poll_wait_remove` is called on every fd to clear stale
task pointers, then the loop repeats.

Fds without `.poll_wait` (no event-driven wakeup) force `sleep_ms` to be
capped at `TICK_MS` (10 ms) so they are polled periodically as a fallback.

`do_select` additionally snapshots the input fd_sets at entry and supports
atomic signal-mask swap for `pselect6`.

See [doc/poll.md](poll.md) for the full algorithm and race-condition analysis.

---

## 8. System Calls (`src/syscall/`)

Dispatched from `syscall.c` via the `int 0x80` handler. The syscall table
follows Linux 2.4 numbering (`include/unistd.h`).

| File             | Syscalls                                                        |
| ---------------- | --------------------------------------------------------------- |
| `syscall_proc.c` | fork, exec, exit, wait, clone, getpid, signal family, mmap/brk  |
| `syscall_fs.c`   | open, read, write, close, stat, lseek, ioctl, getdents, readdir |
| `syscall_net.c`  | socket, bind, connect, listen, accept, send/recv family, ioctl  |
| `syscall_io.c`   | read/write on tty/pipe, ioctl on terminal (TIOCGWINSZ etc.)     |
| `syscall_sys.c`  | uname, gettimeofday, getrlimit, sysinfo, times                  |

---

## 9. Networking (`src/net/`, `src/syscall/syscall_net.c`)

### Stack

MOS integrates **lwIP** (third_party/lwip) in NO_SYS callback mode.

- NIC driver: Intel 82540EM (e1000) via PCI (`src/hw/nic_intel_8254x.c`).
- `net_init()` (`src/net/net.c`) discovers the NIC, configures lwIP with a
  static IP, and brings up the interface.

### Socket types

| Type        | Protocol    | lwIP PCB  |
| ----------- | ----------- | --------- |
| SOCK_STREAM | IPPROTO_TCP | `tcp_pcb` |
| SOCK_DGRAM  | IPPROTO_UDP | `udp_pcb` |
| SOCK_RAW    | any         | `raw_pcb` |

### Raw socket receive layout

lwIP's `raw_input()` calls the recv callback with `p->payload` pointing to
the **IP header** (not stripped). `p->tot_len` = IP header + ICMP payload.
Do **not** prepend the IP header separately — it is already in the pbuf.
Doing so causes double-counting (e.g. 104 bytes instead of 84 for a ping reply).

### Socket ioctl

Common socket ioctls handled in `sock_ioctl()`:

| Code          | Name         | Description                                              |
| ------------- | ------------ | -------------------------------------------------------- |
| 0x8906        | `SIOCGSTAMP` | Timestamp of last received packet (`struct timeval`)     |
| 0x8910–0x8935 | `SIOCGIFxxx` | Interface info: flags, addr, netmask, hwaddr, MTU, index |

---

## 10. Hardware Drivers (`src/hw/`)

| File                          | Device                                                  |
| ----------------------------- | ------------------------------------------------------- |
| `hdd.c`                       | IDE hard disk (ATA PIO) with 4096-page write-back cache |
| `nic.c` / `nic_intel_8254x.c` | Intel 82540EM (e1000) NIC                               |
| `pci.c`                       | PCI bus enumeration                                     |
| `keyboard.c`                  | PS/2 keyboard (IRQ1)                                    |
| `vga.c`                       | VGA framebuffer (768×512×32bpp)                         |
| `serial.c`                    | COM1 serial port (debug output)                         |
| `time.c`                      | PIT (8253/8254) timer driver + `time_now_us`            |
| `apic.c`                      | LAPIC / IOAPIC (SMP)                                    |
| `acpi.c`                      | ACPI MADT parser (CPU and IOAPIC discovery)             |
| `cpu.c`                       | Per-CPU struct init, AP startup                         |

### Timer (`time.c`)

- **PIT:** channel 0, rate mode, HZ=100 (10 ms ticks), LATCH=11932 at
  CLOCK_TICK_RATE=1193180 Hz.
- **`time_now_ms()`:** `total_tickets × 10`.
- **`time_now_us()`:** high-resolution — reads the live PIT countdown register
  to interpolate within the current tick. Resolution ≈ 0.84 μs. Guards against
  tick-boundary races with a `do { } while (t1 != t2)` retry loop.
---

## 11. ELF Loader (`src/elf/`)

`exec.c` implements `execve`: parses ELF32 headers, maps PT_LOAD segments,
loads the dynamic linker (`ld-linux.so.2`) at a non-zero base (ld.so cannot
be loaded at address 0), sets up the initial stack with `argc`, `argv`, `envp`,
and the auxiliary vector (`AT_PHDR`, `AT_ENTRY`, `AT_BASE`, etc.), then
transfers control to the entry point.

---

## 12. SMP (infrastructure present, single-CPU in practice)

AP startup code lives in `src/boot/ap_trampoline.S`. The BSP:
1. Parses ACPI MADT to find CPUs and IOAPIC address.
2. Initialises BSP LAPIC, IOAPIC (masked), and per-CPU TSS.
3. Sends INIT/SIPI IPIs to each AP; APs start at physical 0x8000, receive
   parameters from the page at 0x9000, then jump to the high-address kernel.

The design uses virtual-wire mode: the 8259A PIC remains active for external
IRQs; the IOAPIC is used only for IPI delivery.

---

## 13. Key Constants

| Constant             | Value      | Meaning                       |
| -------------------- | ---------- | ----------------------------- |
| `KERNEL_OFFSET`      | 0xC0000000 | Kernel virtual base           |
| `HZ`                 | 100        | PIT interrupts per second     |
| `CLOCK_TICK_RATE`    | 1193180    | PIT oscillator frequency (Hz) |
| `LATCH`              | 11932      | PIT reload value              |
| `PAGE_SIZE`          | 4096       | Page size                     |
| `KHEAP_BEGIN`        | 0xC0700000 | Start of kernel heap          |
| `MAX_CPUS`           | 8          | Maximum SMP CPUs              |
| `USER_STACK_PAGES`   | 128        | User stack size (512 KB)      |
| `TASK_UNMAPPED_BASE` | 0x40000000 | Base of mmap zone             |
| `HDD_CACHE_SIZE`     | 4096 pages | Block cache capacity          |
