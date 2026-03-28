# Kernel Heap Allocator (`malloc` / `free`)

**Source:** `src/lib/kmalloc.c`
**Headers:** `include/lib/klib.h`

---

## Overview

| Component        | Responsibility                                                |
| ---------------- | ------------------------------------------------------------- |
| `malloc(size)`   | Allocate `size` bytes from the kernel heap; 8-byte aligned    |
| `free(ptr)`      | Return allocation to the heap; right-coalesce with neighbour  |
| `zalloc(size)`   | `malloc` + `memset` to zero                                   |
| `kmalloc_init()` | Initialize spinlock + bin sentinels (called from `klib_init`) |

Aliases in `klib.h`:
```c
#define kmalloc(size) malloc(size)
#define kfree(p)      free(p)
#define kzalloc(size) zalloc(size)
```

---

## 1. Heap address space

```
KHEAP_BEGIN = 0xC0700000
KHEAP_END   = KHEAP_BEGIN + 0x004FF000  (~5 MB static window)
```

The allocator maintains `cur_block_top` starting at `KHEAP_BEGIN`.  When a new chunk is needed:

- **Within the static window** (`cur_block_top + pages < KHEAP_END`): bump `cur_block_top` by `pages * PAGE_SIZE`; the physical pages backing this range are already identity-mapped by the boot page tables.
- **Beyond the static window**: call `vm_alloc(pages)` — allocates physical pages via `phymm_alloc_kernel`, maps them with `mm_kmap_page` at `phys + KERNEL_OFFSET`, and returns the virtual address.

Each call to `extend_heap(min_sz)` allocates the ceiling number of 4 KB pages that covers `min_sz + HDR_SZ`.

---

## 2. Block layout

### Allocated block

```
┌──────────┬─────────────────────────────────┐
│ hdr (4B) │  user data  (ALIGN8(n) bytes)   │
└──────────┴─────────────────────────────────┘
```

- `hdr = total_block_size | ALLOC_BIT`
- `total_block_size = HDR_SZ + ALIGN8(n)`  (`HDR_SZ = 4`, `ALIGN8` rounds up to 8)
- `malloc(n)` returns `blk + 4`; `free(p)` derives `blk = p - 4`.

### Free block (minimum 12 bytes)

```
┌──────────┬──────────┬──────────┬────────────┐
│ hdr (4B) │ next (4) │ prev (4) │  padding   │
└──────────┴──────────┴──────────┴────────────┘
```

- `hdr = total_block_size` (ALLOC_BIT clear)
- `next` / `prev` are pointers into the segregated free-list doubly-linked list.
- `MIN_BLK = 12` — the minimum block size that can hold the free-list pointers.

### Chunk epilogue

Each heap chunk (obtained from `extend_heap`) ends with a 4-byte sentinel:

```c
*(unsigned *)(addr + chunk - HDR_SZ) = ALLOC_BIT;  // size=0, ALLOC_BIT set
```

`blk_next(b)` following the last real block in a chunk lands on this sentinel.  Since `blk_is_alloc(sentinel)` is true, `coalesce_right` stops at the chunk boundary and never merges across chunks.

---

## 3. Segregated free-list bins

`NUM_BINS = 16` bins, each a **circular doubly-linked list** with a sentinel head node (`bin_t`):

```c
typedef struct { unsigned h; void *next, *prev; } bin_t;
static bin_t bins[NUM_BINS];
```

The sentinel's `next`/`prev` fields are at the same offsets as a real free block, so `fl_next`/`fl_prev` work uniformly.

### Bin assignment (`size_to_bin`)

| bin index | holds blocks of total size |
| --------- | -------------------------- |
| 0         | ≤ 12 (MIN_BLK)             |
| 1         | ≤ 24                       |
| 2         | ≤ 48                       |
| …         | (double each step)         |
| 14        | ≤ 98304                    |
| 15        | > 98304 (catch-all)        |

---

## 4. `malloc(size)`

```
1. need = ALIGN8(size + HDR_SZ)
   if need < MIN_BLK: need = MIN_BLK

2. Acquire heap_lock (spinlock)

3. find_free(need):
   a. Start at bin[size_to_bin(need)]; first-fit scan for blk_sz(b) >= need
   b. If not found: scan higher bins — any block there is guaranteed >= need,
      take the first one (O(1))
   c. If still not found: extend_heap(need) → get a new chunk from OS

4. Split if remainder >= MIN_BLK:
     set_hdr(blk, need, ALLOC)
     set_hdr(blk + need, rem, FREE) → fl_insert(lo) into its bin

5. Update heap_quota; release heap_lock

6. Return blk + HDR_SZ
```

Complexity: **O(k)** where k = number of blocks scanned in the starting bin before finding a fit (typically small due to segregation).

---

## 5. `free(ptr)`

```
1. blk = ptr - HDR_SZ
2. Acquire heap_lock
3. heap_quota -= blk_sz(blk)
4. Mark block free: set_hdr(blk, sz, FREE)
5. coalesce_right(blk):
     next = blk_next(blk)
     if !blk_is_alloc(next):
         fl_remove(next)
         set_hdr(blk, blk_sz(blk) + blk_sz(next), FREE)
6. fl_insert(blk) into its bin
7. Release heap_lock
```

Only **right-coalescing** is implemented (no left-coalesce / footer). `free` is **O(1)**.

---

## 6. Instrumentation

```c
unsigned int heap_quota;       // current live bytes (incremented in malloc, decremented in free)
unsigned int heap_quota_high;  // high-water mark
unsigned int heap_time;        // (reserved, not currently used)
```

`heap_quota` is exported in `klib.h` and can be read at any time to measure kernel heap pressure.

---

## 7. User-space heap: `sys_brk`

User-space `malloc` (e.g. glibc) manages its own heap using `sys_brk` (syscall 45):

```c
int sys_brk(unsigned _top);
```

The kernel tracks two per-task values:

| Field                   | Meaning                                                               |
| ----------------------- | --------------------------------------------------------------------- |
| `task->user->start_brk` | Base of the heap segment (set by `elf_map`, = PAGE_ALIGN_UP(BSS end)) |
| `task->user->brk`       | Current heap break (top of the mapped heap)                           |

**`sys_brk` behaviour:**

```
top == 0:
    Ensure at least one page is mapped (lazy-init on first call).
    Return current brk.

top > current brk:
    Expand: do_mmap(brk, ceil_pages * PAGE_SIZE, PROT_READ|PROT_WRITE,
                    MAP_FIXED | MAP_ANONYMOUS)
    brk += pages * PAGE_SIZE
    Return new brk.

top < current brk:
    Shrink: do_munmap(top & PAGE_MASK, pages * PAGE_SIZE)
    brk = top
    Return new brk.

top >= USER_HEAP_END:
    Refused (ENOMEM-equivalent): return current brk unchanged.
```

`USER_HEAP_END = TASK_UNMAPPED_BASE` — the same ceiling that limits mmap growth.

**`MAP_FIXED` on expansion:** the growth `do_mmap` uses `MAP_FIXED` to guarantee
the mapping lands exactly at `brk`.  Without it, if a prior shrink left stale
regions in the heap range (e.g. due to a multi-region unmap), `do_mmap_kernel`
would silently fall back to `vm_disc_map` and place the mapping elsewhere —
advancing `brk` into an unmapped region and causing a page fault on the next
heap access.

**Multi-region shrink:** `do_munmap` correctly handles the case where the shrink
range spans several vm_regions (common when glibc re-grows the heap after a
partial release).  Each overlapping region is split at the unmap boundary;
physical pages outside `[top, old_brk)` are left intact.

Glibc calls `sys_brk(0)` at startup to discover the initial break, then extends
it in page-sized increments via repeated `sys_brk` calls.

---

## 8. Initialization

```
stage2.c → klib_init() → kmalloc_init()
                          ├── spinlock_init(&heap_lock)
                          └── bins_init()  ← each bin[i].next = bin[i].prev = &bins[i]
```

`kmalloc_init` is called very early in stage 2, immediately after TTY init, making `malloc`/`free` available to all subsequent kernel subsystems.
