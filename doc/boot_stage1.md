# Boot Stage 1

## Overview

Stage 1 is the first C code that runs after the CPU enters protected mode via
GRUB. It runs entirely at **physical addresses** (paging is not yet enabled),
sets up the hardware baseline (GDT, IDT, PIC), establishes an initial page
table that maps the first 8 MB of physical memory, enables paging, then
transitions EIP and ESP to virtual (high) addresses before handing off to
`kmain_startup()`.

All stage-1 functions are placed in the `.multiboot` ELF section
(`__attribute__((section(".multiboot")))`), which the linker places at the
start of the kernel image at physical address **0x100000**.

---

## GRUB / Multiboot Machine State at Entry

GRUB hands control to the kernel entry point with the following guaranteed CPU state (Multiboot specification):

| Register / Resource      | Value / Requirement                                                                   |
| ------------------------ | ------------------------------------------------------------------------------------- |
| `EAX`                    | `0x2BADB002` — Multiboot magic; confirms a compliant boot loader                      |
| `EBX`                    | Physical address of the `multiboot_info_t` structure                                  |
| `CS`                     | 32-bit read/execute code segment, base 0, limit 0xFFFFFFFF (exact selector undefined) |
| `DS` `ES` `FS` `GS` `SS` | 32-bit read/write data segments, base 0, limit 0xFFFFFFFF (exact selectors undefined) |
| `CR0`                    | PG (bit 31) = **0** (paging off); PE (bit 0) = **1** (protected mode on)              |
| `EFLAGS`                 | VM (bit 17) = **0**; IF (bit 9) = **0** (interrupts disabled)                         |
| A20 gate                 | Enabled                                                                               |

The following are explicitly **undefined** and must be set up by the kernel before use:

| Resource | Constraint                                                                                                   |
| -------- | ------------------------------------------------------------------------------------------------------------ |
| `ESP`    | No valid stack — kernel must establish one immediately                                                       |
| `GDTR`   | May point to a transient GRUB GDT; kernel must not reload any segment register until it installs its own GDT |
| `IDTR`   | Invalid — interrupts must remain disabled until the kernel loads its own IDT                                 |

All other registers (EDI, ESI, EBP, EDX, ECX, general flags) are undefined.
The PIC is left in the BIOS/DOS state (IRQ0 → vector 8), which the kernel must remap before enabling any IRQ.

---

## Entry point: `entry.S` → `__start`

```
GRUB loads kernel, sets EAX = 0x2BADB002, EBX = multiboot_info_t *
  │
  └─ __start   (entry.S, .multiboot section)
       1. Compute physical stack top: %esp = &stack_top − 0xC0000000
       2. Push EAX (magic) and EBX (multiboot pointer)
       3. call boot_stage1     ← never returns
```

The 16 KB BSS stack (`stack_bottom` / `stack_top`) is defined in `.bss`.
Because paging is off, the virtual address of `stack_top` is adjusted to its
physical equivalent by subtracting `KERNEL_OFFSET` (0xC0000000).

---

## `boot_stage1()` — step by step

```c
_START void boot_stage1(multiboot_info_t *mb, unsigned int magic)
```

All addresses used inside stage-1 functions are physical. The macro
`GET_BOOT_ADDR(type, addr)` converts a linked virtual address to its physical
counterpart: `(type)((unsigned)addr − KERNEL_OFFSET)`.

### Step 1 — Verify Multiboot magic

```c
if (magic != MULTIBOOT_BOOTLOADER_MAGIC)   // 0x2BADB002
    return;
```

Aborts silently if not booted by a Multiboot-compliant loader.

### Step 2 — Copy kernel command line

The Multiboot `cmdline` field points to a physical string. It is copied into
the global `g_cmdline` buffer (accessed via its physical address) for later
parsing by `parse_kernel_cmdline()` once the kernel is running at high
addresses.

### Step 3 — `init_interrupt()` — reprogram the 8259A PIC

The BIOS leaves PIC vector offsets conflicting with CPU exception vectors
(IRQ0 maps to vector 8 = double fault). The PIC is reinitialised:

| ICW  | Master (0x20/0x21)                      | Slave (0xA0/0xA1)          |
| ---- | --------------------------------------- | -------------------------- |
| ICW1 | 0x11 — init + expect ICW4               | same                       |
| ICW2 | 0x20 — remap IRQ0–7 → vectors 0x20–0x27 | 0x28 — IRQ8–15 → 0x28–0x2F |
| ICW3 | 0x04 — slave on IR2                     | 0x02 — slave ID = 2        |
| ICW4 | 0x01 — 8086 mode                        | same                       |
| mask | 0xFF — mask all IRQs                    | 0xFF — mask all IRQs       |

All IRQ lines stay masked until `int_enable_all()` is called later in
`kmain_startup()`.

### Step 4 — `setup_gdt()` — build and load the GDT

A flat 32-bit GDT is written to the physical address of the `gdt[]` array:

| Index | Selector | Descriptor                              |
| ----- | -------- | --------------------------------------- |
| 0     | 0x00     | Null                                    |
| 1     | 0x08     | Kernel data — base 0, limit 4 GB, DPL 0 |
| 2     | 0x10     | Kernel code — base 0, limit 4 GB, DPL 0 |
| 3     | 0x1B     | User data — base 0, limit 4 GB, DPL 3   |
| 4     | 0x23     | User code — base 0, limit 4 GB, DPL 3   |
| 5     | 0x28     | TSS (temporary, limit 0x67, DPL 0)      |

`SET_GDT` loads the GDTR. `SET_CS(0x10)` far-jumps to reload CS.
`SET_DS(0x08)` reloads DS/ES/FS/GS/SS.

### Step 5 — `setup_idt()` — build the IDT

256 interrupt gate descriptors are written to the physical address of `idt[]`.
The stub addresses come from `intr_stubs[]` (populated by `src/int/int.S`).
The syscall stub (`syscall_handler`) is installed at vector 0x80 explicitly.

The IDT is not loaded here — `SET_IDT` (which also calls `sti`) is called
later from `int_enable_all()`. Interrupts remain disabled during the rest of
stage 1.

### Step 6 — `mm_get_phy_mem_bound()` — discover physical RAM

Walks the Multiboot memory map (flag bit 6) scanning all type-1 (usable RAM)
entries:

- `mem_low` ← base of the first non-zero usable region (typically 0x100000,
  i.e. 1 MB — the start of extended memory).
- `mem_high` ← end of the highest usable region (total physical RAM extent).

### Step 7 — `mm_setup_beginning_8m()` — build the initial page tables

```
GDT_ADDRESS = 0x1C0000  (physical)
```

The page directory is placed at `GDT_ADDRESS`. Immediately after it, page
tables are laid out contiguously (one per 4 MB of mapped space).

The mapping covers the physical range **0x000000 – 8 MB** and is installed
at **two** virtual regions simultaneously:

| PDE index | Virtual range           | Maps to                                    |
| --------- | ----------------------- | ------------------------------------------ |
| 0         | 0x00000000 – 0x007FFFFF | phys 0x000000 – 0x7FFFFF (identity map)    |
| 768       | 0xC0000000 – 0xC07FFFFF | phys 0x000000 – 0x7FFFFF (kernel high map) |

The identity mapping (PDE[0]) allows stage-1 code to keep running after
paging is enabled before EIP is redirected to the high address. It is removed
later by `mm_del_user_map()` in `kmain_startup()`.

`RESERVED_PAGES` = `(PAGE_TABLE_CACHE_END − KERNEL_OFFSET) / PAGE_SIZE`
determines how many page tables are pre-populated.

### Step 8 — Enable paging

```c
SET_CR3(GDT_ADDRESS);    // load physical page directory address into CR3
ENABLE_PAGING();         // set CR0.PG
```

After `ENABLE_PAGING()`, virtual address translation is active. EIP still
holds a low (physical-range) address, which works because of the identity
mapping installed in PDE[0].

### Step 9 — Transition to high virtual addresses

```c
RELOAD_EIP();   // indirect jump through a high-address label
RELOAD_ESP();   // ESP += KERNEL_OFFSET  (0xC0000000)
```

`RELOAD_EIP()` performs an indirect jump via a computed high address:
`movl $label, %eax; jmp *%eax`, redirecting EIP into the kernel's virtual
window at `0xC0xxxxxx`.

`RELOAD_ESP()` adds `KERNEL_OFFSET` to the current ESP, mapping the stack
pointer into the high window. From this point all addresses are virtual.

### Step 10 — Initialise the physical memory allocator

```c
phymm_end   = mem_high / PAGE_SIZE;
phymm_begin = phymm_get_mgmt_pages(mem_high)    // pages needed for phymm_pages[]
            + mem_low / PAGE_SIZE
            + RESERVED_PAGES;                    // skip boot-reserved pages
```

`phymm_begin` is the first page index the allocator may hand out;
`phymm_end` is the exclusive upper bound.

```c
mm_init_page_table_cache();         // set up the page-table slab
phymm_setup_mgmt_pages(...);        // map & zero phymm_pages[]
phymm_init(mmap_addr, mmap_len);    // mark holes, build buddy free lists
```

The physical allocator uses a **buddy system** (`phymm.c`): free pages are
organised into doubly-linked lists indexed by order (0 = 4 KB, 1 = 8 KB, …
up to `MAX_BUDDY_ORDER`). Allocation coalesces and splits blocks as needed.

### Step 11 — Hand off to `kmain_startup()`

```c
kmain_startup();   // never returns
```

Stage 1 is complete. The CPU is now running at ring 0, paging is enabled,
GDT and IDT are set up (IDT not yet loaded), physical memory is managed, and
the kernel heap is ready.

---

## Summary of address space at end of stage 1

```
Physical:                          Virtual (after RELOAD_EIP/ESP):
0x000000  BIOS / low memory        0x00000000  identity map (temporary)
0x100000  kernel image             0xC0200000  kernel image
0x1C0000  page directory           0xC01C0000  page directory
0x1C1000  page tables…             0xC01C1000  page tables…
                                   0xC0700000  kernel heap (KHEAP_BEGIN)
                                   0xC0C00000  page table cache
```
