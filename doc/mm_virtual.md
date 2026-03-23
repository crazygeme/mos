# Virtual Memory Management

**Source:** `src/mm/mm.c`, `src/mm/mmap.c`, `src/mm/pagefault.c`
**Headers:** `include/mm/mm.h`, `include/mm/mmap.h`

---

## Overview

Virtual memory is managed across three layers:

| Layer | File | Responsibility |
|-------|------|---------------|
| Page table management | `mm.c` | PDE/PTE manipulation, kernel heap, mapping primitives |
| VM region map | `mmap.c` | Per-process address space descriptor, `mmap`/`munmap` |
| Page fault handler | `pagefault.c` | Demand paging, CoW, file-backed faults, page cache |

---

## 1. Address space layout

```
0x00000000 – 0x3FFFFFFF   user text / data / heap  (grows upward via brk)
0x40000000 – USER_ZONE_END  mmap zone  (TASK_UNMAPPED_BASE)
USER_ZONE_END – 0xBFFFFFFF  user stack (grows downward, 128 pages)
0xC0000000 – 0xC06FFFFF  kernel image + direct-mapped RAM
0xC0700000 – 0xC0BFEFFF  kernel heap  (KHEAP_BEGIN / KHEAP_END)
0xC0C00000 – 0xC4BFFFFF  page table cache (1024 × 4 KB)
```

`KERNEL_OFFSET` = 0xC0000000.
`KERNEL_PAGE_DIR_OFFSET` = 0xC0000000 / 4 MB = PDE index 768.

---

## 2. Page table management (`mm.c`)

### Two-level x86 paging

```
CR3 → page directory (1024 PDEs, each covering 4 MB)
           │
           └─ page table (1024 PTEs, each covering 4 KB)
                  │
                  └─ physical page
```

Address decomposition macros:

```c
ADDR_TO_PGT_OFFSET(addr)  // bits [31:22] → PDE index (0-1023)
ADDR_TO_PET_OFFSET(addr)  // bits [21:12] → PTE index (0-1023)
```

### PTE flag bits

| Flag | Bit | Meaning |
|------|-----|---------|
| `PAGE_ENTRY_PRESENT` | 0 | Page is valid |
| `PAGE_ENTRY_WRITABLE` | 1 | Writable |
| `PAGE_ENTRY_DPL_USER` | 2 | User-accessible |
| `PAGE_ENTRY_WT` | 3 | Write-through cache |
| `PAGE_ENTRY_CD` | 4 | Cache disable |

Composite flags:

```c
PAGE_ENTRY_KERNEL_CODE  = PRESENT
PAGE_ENTRY_KERNEL_DATA  = PRESENT | WRITABLE
PAGE_ENTRY_USER_CODE    = PRESENT | DPL_USER
PAGE_ENTRY_USER_DATA    = PRESENT | WRITABLE | DPL_USER
```

### Page table cache

Page tables are allocated from a **static stack cache** rather than `kmalloc`,
to avoid allocation inside fault handlers.

```
Region: [PAGE_TABLE_CACHE_BEGIN, PAGE_TABLE_CACHE_END)
         0xC0C00000 – 0xC4BFFFFF (4 MB, 1024 page tables)

page_table_cache_t {
    unsigned top;         // stack top index
    unsigned count;       // free slots remaining
    unsigned mem[1024];   // virtual addresses of free page tables
}
pgc_entry_count[1024]    // live PTE count per table (for reclaim)
```

- `mm_alloc_page_table()` — pops from the stack, zeroes the page, returns vaddr
- `mm_free_page_table(vir)` — pushes vaddr back; called automatically when
  `pgc_entry_count` reaches 0 (all PTEs cleared → table is reclaimed)

### Core internal helper: `mm_get_valid_page_table`

```c
static int mm_get_valid_page_table(unsigned addr, unsigned flag,
                                   mm_addr_info *info, int alloc_if_none);
```

Walks the two-level table for `addr`. If the PDE is absent and `alloc_if_none`
is set, allocates a new page table. Fills `mm_addr_info`:

```c
typedef struct {
    unsigned *dir;    // pointer to the PDE
    unsigned *table;  // base of the page table
    unsigned *entry;  // pointer to the PTE
} mm_addr_info;
```

### Mapping primitives

| Function | Description |
|----------|-------------|
| `mm_add_direct_map(vir)` | Map kernel vaddr → phys = vir − KERNEL_OFFSET; ref-counts the page |
| `mm_del_direct_map(vir)` | Unmap and deref |
| `mm_add_dynamic_map(vir, phy, flag)` | Map user vaddr → phy (allocates a new page if phy=0); ref-counts |
| `mm_del_dynamic_map(vir)` | Unmap user page; frees physical page when ref_count reaches 0 |
| `mm_map_phys_page(phys)` | Map firmware/MMIO phys → virt = phys + KERNEL_OFFSET; no ref-counting |
| `mm_add_resource_map(phy)` | Map high MMIO address (virt = phy) directly |
| `mm_del_user_map()` | Clears PDE[0] and PDE[1] (removes the boot identity map) |
| `mm_get_map_flag(vir)` | Return PTE flags for vaddr |
| `mm_set_map_flag(vir, flag)` | Overwrite PTE flags |
| `mm_get_attached_page_index(vir)` | Return physical page index for vaddr |

### Kernel heap: `vm_alloc` / `vm_free`

```c
unsigned vm_alloc(int page_count);   // alloc contiguous kernel pages; returns vaddr
void     vm_free(unsigned vm, int page_count);
```

`vm_alloc` calls `phymm_alloc_kernel`, maps each page with `mm_add_direct_map`,
flushes the TLB, and returns `page_index * PAGE_SIZE + KERNEL_OFFSET`.

### Pathname buffer cache: `name_get` / `name_put`

A small slab of `MAX_PATH` (4096-byte) buffers, to avoid repeated
`vm_alloc`/`vm_free` for temporary path strings in syscall handlers.

---

## 2. VM region map (`mmap.c`)

### `vm_region` — the mapping descriptor

```c
typedef struct {
    unsigned begin;   // start virtual address (page-aligned)
    unsigned end;     // end virtual address (exclusive, page-aligned)
    int      prot;    // PROT_READ | PROT_WRITE | PROT_EXEC | PROT_NONE
    int      flag;    // MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS
    file    *fp;      // backing file (NULL for anonymous)
    int      offset;  // byte offset into the file
} vm_region;
```

### Data structure: interval tree

Each process's address space is stored in a **hash table used as an interval
tree**, keyed by `vm_key { begin, end }`. The comparator treats any range
overlap as equal (returns 0), so a lookup for address `A` probes with
`{ A, A+PAGE_SIZE }` and finds any region containing `A` in O(log n).

```c
vm_struct_t vm_create();              // allocate a new VM map
void        vm_destroy(vm_struct_t);  // free all regions
```

### `vm_add_map` — insert a region

Inserts `[begin, end)` with `prot`/`flag`/`fp`/`offset`. If the new range
overlaps any existing region, the existing region is split:

```
Before: [====existing====]
Insert:        [+++new++++++]
After:  [==left] [+++new++++++]   (right remnant if needed)
```

The conflict-resolution loop runs until no overlapping region remains.
`fs_get_file` is called on `fp` to hold a reference for the lifetime of the
region.

### `vm_del_map` — remove a region

Finds the region containing `addr`, unmaps every page via `mm_del_dynamic_map`,
reloads CR3, and removes the descriptor. Calls `fs_put_file` on the backing
file.

### `vm_find_map` — address lookup

```c
vm_region *vm_find_map(vm_struct_t vm, unsigned addr);
```

Returns the `vm_region` containing `addr`, or NULL.

### `vm_disc_map` — find a free virtual range

Scans existing regions in order and returns the first gap in
`[TASK_UNMAPPED_BASE, KERNEL_OFFSET)` large enough to hold `size` bytes.
Used by `do_mmap_kernel` when no hint address is given.

### `do_mmap_kernel` — core mmap implementation

1. Page-aligns `addr` and `len`.
2. If `MAP_FIXED`: use `addr` exactly (vm_add_map replaces any overlap).
3. Otherwise: if the hint is occupied or zero, call `vm_disc_map` to find a
   free range.
4. Calls `vm_add_map` to register the region. **No physical pages are
   allocated** — pages are faulted in lazily.

### `do_munmap` — unmap a range

Supports partial unmap (unmapping a sub-range of an existing region). The
containing region is split: portions outside the unmapped range are
re-inserted as independent regions.

Before removing pages, `vm_flush_dirty_region` writes back any dirty
`MAP_SHARED` pages to the backing file.

### `vm_mprotect` — change protection

Updates `prot` for `[begin, end)` without unmapping pages. Overlapping
regions are split and the intersecting portion is re-inserted with `new_prot`.
File references are bumped before `hash_remove` and released after re-insert
to avoid use-after-free.

### `vm_dup` — fork address space

Copies all `vm_region` descriptors from parent to child. Physical pages are
**not** copied — the parent's pages become CoW (handled at the page-table
level by `fork()` marking all PTEs read-only).

### `vm_flush_all_dirty` — process exit flush

Iterates every region and writes dirty `MAP_SHARED` file-backed pages back to
disk via `ext4_fwrite` before the VM is torn down.

---

## 3. Page fault handler (`pagefault.c`)

Registered at vector 14 (`pf_init`). Runs with interrupts **enabled** (disk
reads may be needed).

### Error code bits

```
bit 0  P   — 0 = page not present, 1 = protection violation
bit 1  W/R — 0 = read fault, 1 = write fault
bit 2  U/S — 0 = kernel mode, 1 = user mode
```

### Dispatch in `pf_process`

```
error & P == 0  (not present)  → pf_handle_page_invalid(cr2)
error & P == 1  (present)
  error & W == 1  (write fault) → pf_handle_permission(cr2)
  otherwise                     → unhandled (SIGSEGV / kernel panic)
```

`cr2` is masked to the page boundary before dispatch.

### `pf_handle_page_invalid` — demand paging

1. Look up `cr2` in the current task's VM map (`vm_find_map`).
2. If no region found, or `prot == PROT_NONE`: return 0 → SIGSEGV.
3. If region has a backing file → `pf_handle_invalid_file_map` (major fault).
4. Otherwise → `pf_handle_invalid_memory` (minor fault).

#### Anonymous fault: `pf_handle_invalid_memory`

- **Writable** (`PROT_WRITE`): allocate a fresh physical page, map
  `PAGE_ENTRY_USER_DATA`, zero it.
- **Read-only**: map the shared **zero page** as `PAGE_ENTRY_USER_CODE`
  (read-only). The zero page's `ref_count` becomes > 1, so the next write
  triggers CoW automatically.

#### File-backed fault: `pf_handle_invalid_file_map`

1. **Page cache lookup** `mmap_cache_find(ino, offset)` — O(1) hash lookup.
   - Hit: map the cached physical page directly (read-only). No disk I/O.
2. **Cache miss**: allocate a physical page, map it writable, read `PAGE_SIZE`
   bytes from the file via `ext4_fread`. Zero-fill any short read.
3. Downgrade the PTE to **read-only** after the read (so write faults trigger
   CoW or `MAP_SHARED` write-back, not silent corruption of the cache).
4. Insert the page into the cache via `mmap_cache_add`.

### `pf_handle_permission` — write fault on a present page

Dispatches based on region flags:

| Condition | Action |
|-----------|--------|
| `MAP_SHARED` | Flip PTE writable in-place (`pf_handle_readonly`). If file-backed, mark physical page dirty (`phymm_mark_dirty`). |
| `MAP_PRIVATE`, `ref_count > 1` | CoW: allocate a private copy (`pf_handle_cow`). |
| `MAP_PRIVATE`, `ref_count == 1` | Sole owner; just flip PTE writable (`pf_handle_readonly`). |
| No covering region, or `PROT_WRITE` not set | Return 0 → SIGSEGV. |

#### CoW sequence: `pf_handle_cow`

```
1. vm_alloc(1)            → allocate new kernel-mapped page
2. memcpy(new, cr2, 4KB)  → copy the page contents
3. mm_del_dynamic_map(cr2) → release old physical page (deref; may free it)
4. phymm_reference_page    → pin new page so it isn't freed before remapping
5. mm_del_direct_map       → unmap new page from kernel window
6. mm_add_dynamic_map(cr2, new_phy, writable) → install private copy
7. phymm_dereference_page  → release the extra pin
8. RELOAD_CR3              → flush TLB
```

### Page cache: LRU hash table

```c
static hash_table *mmap_cache;   // keyed by (ino, offset)
static list_entry  mmap_lru;     // tail = MRU, head = LRU
```

Capacity: `PAGE_CACHE_SIZE` = 4096 entries.
On miss + full: evict the LRU entry (head of `mmap_lru`), call
`phymm_dereference_page` on its physical page.
On hit: promote entry to tail (MRU position).

A global **zero page** is allocated at `pf_init` time and reused for all
anonymous read-only faults to avoid allocating a new page per `mmap` region.

### Instrumentation counters

| Counter | Meaning |
|---------|---------|
| `page_fault_cow` | CoW faults |
| `page_fault_invalid` | Anonymous demand-page faults |
| `page_fault_file` | File-backed demand-page faults |
| `page_fault_file_cache_hit` | File faults served from page cache |
| `page_fault_perm` | Permission-upgrade (read-only → writable) faults |
| `page_fault_file_read` | Total bytes read from disk for file faults |

Cycle-accurate timing is collected when `TestControl.profiling == 1`.

---

## 4. Lifecycle summary

```
mmap(addr, len, prot, flags, fd, offset)
  └─ do_mmap → do_mmap_kernel
       └─ vm_add_map   [registers descriptor; no pages allocated]

first access to mapped page
  └─ #PF (vector 14) → pf_process
       ├─ page not present → pf_handle_page_invalid
       │    ├─ anonymous  → allocate page, zero, map writable (or share zero page)
       │    └─ file-backed → cache lookup → read from ext4 → map read-only → cache insert
       └─ write to read-only page → pf_handle_permission
            ├─ MAP_SHARED  → flip PTE writable; mark dirty if file-backed
            └─ MAP_PRIVATE → CoW if shared, else flip PTE writable

munmap(addr, length)
  └─ do_munmap
       ├─ vm_flush_dirty_region  [write back dirty MAP_SHARED pages]
       ├─ vm_del_map             [unmap PTEs, deref physical pages]
       └─ re-insert remnants     [split if partial unmap]

fork()
  └─ vm_dup   [copy vm_region descriptors to child]
  └─ (caller) mark all PTEs read-only in both parent and child
       └─ first write → CoW fault → pf_handle_cow

process exit
  └─ vm_flush_all_dirty   [flush all dirty MAP_SHARED pages]
  └─ vm_destroy           [free all vm_region descriptors]
```
