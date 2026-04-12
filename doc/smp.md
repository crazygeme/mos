# SMP Notes

This document summarizes the SMP-related work currently implemented in MOS, with emphasis on CPU bring-up, cross-CPU scheduling, and the current kernel page-table synchronization model.

## Current Status

MOS can bring up multiple CPUs on x86 with LAPIC + IOAPIC support and run per-CPU idle tasks plus normal schedulable work. The current SMP support is functional enough for cross-CPU scheduling and basic TLB shootdown, but the kernel mapping model is still expensive under fork/COW-heavy workloads.

The main implemented pieces are:

- ACPI-based CPU discovery
- BSP/AP Local APIC initialization
- AP startup with INIT + SIPI + SIPI
- Per-CPU TSS storage
- Per-CPU ready queues and timer queues
- Cross-CPU scheduler kick IPI
- TLB shootdown IPI
- Linux-like static kernel PDE template copied once into each task pgdir

## Boot And CPU Bring-Up

High-level flow:

1. Stage 1 enables paging with the bootstrap page directory.
2. Stage 2 initializes core kernel subsystems.
3. ACPI MADT is parsed to discover LAPIC IDs and IOAPIC base.
4. The BSP initializes LAPIC/IOAPIC.
5. AP trampoline code is copied into low physical memory.
6. The BSP sends INIT + SIPI + SIPI to each AP.
7. Each AP enables paging using the shared bootstrap CR3, enters C code, initializes its LAPIC/TSS/IDT state, and waits for scheduler release.

Relevant files:

- [src/boot/stage2.c](/home/zhengjia/project/mos/src/boot/stage2.c)
- [src/boot/ap_trampoline.S](/home/zhengjia/project/mos/src/boot/ap_trampoline.S)
- [src/hw/cpu.c](/home/zhengjia/project/mos/src/hw/cpu.c)
- [src/hw/apic.c](/home/zhengjia/project/mos/src/hw/apic.c)

## Scheduler SMP Behavior

Each CPU has:

- its own ready queues
- its own timer RB-tree
- its own idle task
- its own TSS

Tasks have an affinity, and enqueueing a task onto another CPU's ready queue sends `IPI_VECTOR_SCHED` so an idle target CPU wakes from `HLT` and reruns `task_sched()`.

Relevant files:

- [src/ps/ps_sched.c](/home/zhengjia/project/mos/src/ps/ps_sched.c)
- [src/ps/ps.c](/home/zhengjia/project/mos/src/ps/ps.c)
- [src/int/int.c](/home/zhengjia/project/mos/src/int/int.c)

## CR3 And Address Spaces

On x86, `CR3` is a per-CPU register. MOS stores a page-directory base per task, then loads that task's `CR3` during context switch.

This means:

- hardware `CR3`: per CPU
- `task->cr3` / `task->user->page_dir`: per task
- kernel mappings: intended to be identical across tasks in the kernel half of the page directory

Relevant files:

- [src/ps/ps_sched.c](/home/zhengjia/project/mos/src/ps/ps_sched.c)
- [src/ps/ps.c](/home/zhengjia/project/mos/src/ps/ps.c)

## Kernel Mapping Model

Each task has its own page-directory page (user half differs per process). The kernel half follows the same model as Linux 32-bit: a single master template (`kernel_pde_template[]`) holds the canonical kernel PDEs, and all tasks point to the **same physical page-table pages** for kernel space.

What is shared physically:

- kernel page-table pages (PT pages for addresses ≥ `KERNEL_OFFSET`)
- all kernel PTE updates inside those shared page tables — visible to every task that holds the PDE

What is per-task:

- the page-directory page itself
- the kernel PDEs copied into it (pointing to the shared PT pages)

### Lifecycle

1. **Boot** — `mm_init_page_table_cache()` copies existing kernel PDEs from the boot pgdir into `kernel_pde_template[]`, then pre-allocates a PT page for every remaining empty kernel PDE slot (at most 256 in total, 1 MB).  After this one-time loop the template is **fully populated and never changes**.

2. **Task creation** — `mm_init_task_pagedir()` copies all 256 kernel PDEs from `kernel_pde_template[]` into the new pgdir.  Because the PT pages are shared, every kernel mapping made before or after this point is immediately visible through the shared PTEs — no further synchronisation is needed.

3. **New kernel PTE** (`vm_alloc` / `mm_kmap_page`) — writes into the shared PT page.  The PDE already exists in every task's pgdir, so the new mapping is visible everywhere without any IPI or PDE propagation.

4. **Kernel PT pages are never reclaimed** — freeing a kernel PT page while other tasks hold a PDE pointing at it would be a use-after-free.  An empty kernel PT page costs one 4 KB cache slot; the cost is bounded to 256 × 4 KB = 1 MB.

Relevant file:

- [src/mm/mm.c](/home/zhengjia/project/mos/src/mm/mm.c)

## TLB Shootdown

MOS has a broadcast TLB shootdown IPI:

- sender: `smp_tlb_shootdown()`
- vector: `IPI_VECTOR_TLB`
- receiver: `ipi_tlb_handler()` — reloads `CR3`

No PDE sync is needed in the handler because stale PDEs are handled lazily by the vmalloc fault path; the shootdown is purely for flushing stale PTE translations after a kernel mapping is removed.

Relevant files:

- [src/hw/cpu.c](/home/zhengjia/project/mos/src/hw/cpu.c)
- [src/int/int.c](/home/zhengjia/project/mos/src/int/int.c)

## Fixes Added During SMP Bring-Up

1. Enqueuing a freshly allocated task on another CPU could fault because the target CPU's pgdir lacked the kernel PDE for the new `task_struct`.  Now handled by vmalloc fault on first access.
2. The earlier per-CPU `kernel_pde_gen`/`kernel_pde_seen` scheme was wrong: it tracked per-CPU rather than per-task state, so a CPU that switched tasks without receiving a TLB IPI would use a pgdir with stale PDEs.  Replaced by the vmalloc fault mechanism.
3. The earlier `ps_copy_kernel_map` hook copied all 256 kernel PDEs on every context switch.  Removed — vmalloc faults handle propagation lazily and only when actually needed.

## Current Limitations

- TLB shootdown is fire-and-forget, not acknowledged.
- Kernel PT pages are never reclaimed (bounded to at most 256 × 4 KB = 1 MB for the full kernel half).

## Recommended Next Steps

1. Add acknowledged TLB shootdown if page-table pages ever need to be freed/reused immediately after invalidation.
2. Consider per-CPU temporary kmap slots if page-copy traffic grows significantly.
