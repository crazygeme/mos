# Boot Stage 2

## Overview

Stage 2 is `kmain_startup()` in `src/boot/stage2.c`. It is called at the end
of stage 1, running at ring 0 with paging enabled and physical memory managed.
Its job is to bring up every kernel subsystem in dependency order, then launch
the initial kernel threads and hand control to the scheduler.

Stage 2 is split into two phases:

1. **`kmain_startup()`** — synchronous init: runs on the original boot stack,
   no scheduling yet. Ends with `ps_kickoff()` which starts the scheduler.
2. **`kmain_process()`** — runs as the first scheduled kernel thread, walks
   the `KERNEL_INIT` table, then becomes the idle loop.

---

## Phase 1 — `kmain_startup()`

### 1. Framebuffer and console (`fb_init`, `fb_enable`, `tty_init`, `klib_init`)

```
fb_init()      — detect and map VGA framebuffer (768×512×32bpp)
fb_enable()    — switch to graphics mode
tty_init()     — initialise TTY layer (line discipline, output buffer)
klib_init()    — initialise kmalloc heap and printk; after this, the full
                 kernel library (kmalloc/kfree/printk/memcpy…) is usable
```

### 2. Command line parsing (`parse_kernel_cmdline`)

Tokenises the `g_cmdline` string (copied from Multiboot in stage 1) into
space/tab-separated tokens. Recognised options populate the global
`TestControl` struct:

| Token         | Effect                                                    |
| ------------- | --------------------------------------------------------- |
| `verbose`     | `TestControl.verbos = 1` — enables extra `klog` output    |
| `profile`     | `TestControl.profiling = 1` — enables scheduler profiling |
| `init=<path>` | `TestControl.bash = 1` — overrides `/sbin/init`           |

### 3. Process subsystem (`ps_init`)

Initialises the MPRQ scheduler data structures: ready/wait/dying queues,
the manager RB-tree, spinlocks (`ps_lock`, `map_lock`, `psid_lock`), and the
TSS. No tasks are runnable yet; scheduling is disabled until `ps_kickoff()`.

### 4. Deferred Service Routines (`dsr_init`)

Sets up the DSR queue (kernel bottom-half mechanism, depth `DSR_CACHE_DEPTH =
100`). DSRs are used by drivers to defer work out of interrupt context.

### 5. Enable interrupts (`int_enable_all`)

Loads the IDT (built in stage 1) and executes `sti`. The 8259A PIC is
unmasked selectively — only the vectors the kernel has registered handlers for.
From this point hardware interrupts are live.

### 6. Remove identity map (`mm_del_user_map` + `RELOAD_CR3`)

Clears PDE[0] (the temporary 0x00000000–0x007FFFFF identity mapping installed
in stage 1 to let EIP survive the paging transition). `RELOAD_CR3()` flushes
the TLB. Low virtual addresses are now unmapped; any access to them will fault.

### 7. Serial port (`serial_init_queue`)

Initialises COM1 with an interrupt-driven transmit queue. `printk` output is
mirrored to the serial port for debugging.

### 8. Keyboard (`kb_init`)

Registers the PS/2 keyboard interrupt handler (IRQ1 → vector 0x21). Key
events are fed into the TTY line discipline.

### 9. PIT timer (`time_init` + `time_calculate_cpu_cycle`)

```
time_init()              — program PIT channel 0 in rate-generator mode:
                           HZ=100, LATCH=11932, CLOCK_TICK_RATE=1193180 Hz.
                           Registers the PIT ISR at vector 0x20 (IRQ0).
time_calculate_cpu_cycle — busy-wait calibration: measures how many loop
                           iterations fit in one PIT tick, used for usleep/delay.
```

The PIT ISR (`time_process`) drives the scheduler time-slice countdown and
signal delivery on every tick.

### 10. Page fault handler (`pf_init`)

Registers the `#PF` handler (vector 14). Handles demand paging, copy-on-write,
and user stack growth. Page faults must run with interrupts enabled (disk I/O
may be needed), so the handler does not disable interrupts globally.

### 11. SMP initialisation (conditional)

```
acpi_parse(&g_acpi_info)   — parse ACPI MADT: discover CPU APIC IDs and
                             IOAPIC base address
```

If more than one CPU is found:

```
apic_init_bsp()            — enable BSP LAPIC in virtual-wire mode
ioapic_init(ioapic_phys)   — map IOAPIC, mask all redirection entries
                             (external IRQs stay with 8259A)
cpu_init_bsp()             — initialise BSP per-CPU struct, load TSS
smp_start_aps()            — send INIT/SIPI IPIs; each AP runs the
                             trampoline at phys 0x8000, receives params
                             from phys 0x9000, then calls ps_kickoff_ap()
```

**Virtual-wire mode:** the 8259A PIC remains the primary interrupt controller.
Its INTR line passes through BSP LINT0 (ExtINT mode). The LAPIC is enabled
only for IPI delivery (`IPI_VECTOR_SCHED` 0xF0, `IPI_VECTOR_TLB` 0xF1).
`int_set_apic_mode()` is intentionally **not** called — doing so would switch
EOI to the LAPIC, causing the 8259A to block all subsequent PIC interrupts
(including the PIT tick at vector 0x20).

If only one CPU is found, SMP is skipped and a message is printed.

### 12. Create initial kernel threads

```c
if (ncpus == 1)
    ps_create(idle_process,   NULL, ps_idle,   ps_kernel);
ps_create(kmain_process,  NULL, ps_normal, ps_kernel);
ps_create(timer_process,  NULL, ps_normal, ps_kernel);
```

| Thread          | Priority    | Purpose                                                                                          |
| --------------- | ----------- | ------------------------------------------------------------------------------------------------ |
| `idle_process`  | `ps_idle`   | Executes `HLT` in a loop; created only on single-CPU systems (SMP APs have their own idle loops) |
| `kmain_process` | `ps_normal` | Runs the `KERNEL_INIT` table then becomes idle                                                   |
| `timer_process` | `ps_normal` | Runs software timers (`timer_init` + `do_timer_loop`)                                            |

### 13. Start the scheduler (`ps_kickoff`)

```c
ps_kickoff();   // enables scheduling, picks the first task, never returns
                // to this point on the boot stack
run();          // unreachable; falls into idle_process as a safety net
```

`ps_kickoff()` marks the scheduler active and performs the first context
switch. The boot stack is abandoned.

---

## Phase 2 — `kmain_process()` (KERNEL_INIT table)

Runs as a scheduled kernel thread. Iterates `__kinit_start[]` →
`__kinit_end[]`, a linker-collected array of function pointers ordered by the
index argument of each `KERNEL_INIT(index, fn)` macro.

| Index | Function             | What it does                                                                              |
| ----- | -------------------- | ----------------------------------------------------------------------------------------- |
| 0     | `klog_init`          | Opens the kernel log file (`/var/log/kmsg`)                                               |
| 1     | `syslog_init`        | Initialises the in-memory syslog ring buffer                                              |
| 2     | `hdd_init`           | Scans PCI for IDE controllers, initialises ATA driver and 4096-page write-back disk cache |
| 2     | `mount_syscall_init` | Registers `mount`/`umount` syscall handlers                                               |
| 3     | `fs_mount_root`      | Registers the ext2/4 filesystem driver and mounts the root partition (`/dev/hda`) at `/`  |
| 4     | `tty_fs_init`        | Mounts the TTY filesystem, creates `/dev/tty`, `/dev/console`                             |
| 5     | `pts_dev_init`       | Mounts devpts at `/dev/pts`; creates PTY master/slave devices                             |
| 5     | `procfs_init`        | Mounts procfs at `/proc`; exposes `/proc/<pid>/`, `/proc/meminfo`, etc.                   |
| 6     | `devfs_init`         | Mounts devfs at `/dev`; populates static device nodes                                     |
| 6     | `nic_scan_all`       | PCI scan for Intel 82540EM (e1000) NIC; calls `nic_init` per device                       |
| 7     | `net_init`           | Initialises lwIP, configures the NIC interface, brings up IP                              |
| 7     | `syscall_init`       | Installs the `int 0x80` syscall dispatch table                                            |
| 8     | `kinit_userspace`    | Loads and `exec`s the first userspace binary (`/sbin/init` or `init=` override)           |

After `kinit_userspace` returns (it shouldn't — init runs forever), `kmain_process`
falls into `run()` → `idle_process()`.

---

## `timer_process`

Runs as a separate kernel thread alongside `kmain_process`:

```c
timer_init();      // initialise the software timer heap
do_timer_loop();   // block, fire expired timers, repeat
```

Software timers (used by `msleep`, socket timeouts, etc.) are managed
independently of the PIT ISR to avoid doing work in interrupt context.

---

## Initialisation dependency graph

```
klib_init ──────────────────────────────────────── (kmalloc/printk usable)
    │
    ├─ parse_kernel_cmdline
    ├─ ps_init ─────────────────────────────────── (scheduler structures)
    ├─ dsr_init
    ├─ int_enable_all ──────────────────────────── (interrupts live)
    ├─ mm_del_user_map ─────────────────────────── (identity map removed)
    ├─ serial_init_queue
    ├─ kb_init
    ├─ time_init ───────────────────────────────── (PIT ticks, preemption)
    ├─ pf_init ─────────────────────────────────── (demand paging)
    ├─ acpi_parse → apic_init_bsp → ioapic_init → cpu_init_bsp → smp_start_aps
    └─ ps_kickoff ──────────────────────────────── (scheduler running)
           │
           ├─ kmain_process → KERNEL_INIT[0..8]
           │       └─ kinit_userspace → /sbin/init (first userspace process)
           ├─ timer_process → do_timer_loop
           └─ idle_process  → HLT loop
```
