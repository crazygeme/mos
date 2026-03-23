# Physical Memory Management

**Source:** `src/mm/phymm.c`, `include/mm/phymm.h`

---

## Overview

Physical memory is managed by a **buddy allocator**. It tracks every 4 KB
page in a flat array (`phymm_pages[]`) and organises free pages into
per-order doubly-linked free lists, allowing O(log n) allocation and
O(log n) coalescing on free.

---

## Page descriptor

Every physical page has one `phymm_page` entry:

```c
typedef struct _phymm_page {
    unsigned int ref_count;   // 0=free, PHYMM_RESERVED=non-RAM, >0=in use
    unsigned int next_free;   // buddy free list: next page index
    unsigned int prev_free;   // buddy free list: prev page index
    unsigned char order;      // buddy order stored on block head page
    unsigned char flags;      // PHYMM_PAGE_DIRTY
} phymm_page;
```

Special sentinel values:

| Value | Meaning |
|-------|---------|
| `PHYMM_INVALID` (0xFFFFFFFF) | End-of-list / invalid page index |
| `PHYMM_RESERVED` (0xFFFFFFFE) | `ref_count`: page is a non-RAM hole |
| `PHYMM_ORDER_NONE` (0xFE) | `order`: non-head page inside a multi-page block |

`phymm_pages` is a global pointer to the array, mapped into the kernel
virtual window during boot by `phymm_setup_mgmt_pages`.

---

## Global bounds

| Variable | Meaning |
|----------|---------|
| `phymm_begin` | First page index the allocator may hand out |
| `phymm_end` | Exclusive upper bound (highest physical page) |
| `phymm_used` | Count of pages currently with `ref_count > 0` |

`phymm_begin` is set in `boot_stage1` to skip:
1. Pages below the first usable RAM region (`mem_low / PAGE_SIZE`)
2. `RESERVED_PAGES` — kernel image, page directory, and page table cache
3. Management pages — the `phymm_pages[]` array itself

---

## Buddy system

### Data structures

```
buddy_lists[0]  → singly-linked free list of order-0 blocks (4 KB each)
buddy_lists[1]  → order-1 blocks (8 KB)
...
buddy_lists[10] → order-10 blocks (4 MB each, MAX_BUDDY_ORDER = 10)
```

Each list is doubly-linked through `phymm_page.next_free` / `prev_free`,
so removing a specific block (needed during coalescing) is O(1).

The buddy of a block at page index `idx` with order `o` is at:

```
buddy_index = idx XOR (1 << o)
```

### Allocation — `buddy_alloc(order)`

1. Scan `buddy_lists` from `order` up to `MAX_BUDDY_ORDER` for the first
   non-empty list.
2. Pop the head block from that list.
3. If the block is larger than requested, **split** it repeatedly: the upper
   half of each split is pushed back onto the appropriate free list.
4. The head page stores the allocated `order`; non-head pages within the
   block get `order = PHYMM_ORDER_NONE`.

```
Example: allocate order 1 (8 KB), only order-3 free block exists at idx 0

buddy_lists[3]: [0]         (block covers pages 0-7)
                ↓ pop, split order 3 → 2
buddy_lists[2]: [4]         (pages 4-7 pushed back)
                ↓ split order 2 → 1
buddy_lists[1]: [2]         (pages 2-3 pushed back)
result: idx=0, order=1      (pages 0-1 allocated)
```

### Free + coalesce — `buddy_free_block(idx, order)`

1. Compute buddy index: `buddy = idx XOR (1 << order)`.
2. If buddy is free at the same order (same `ref_count == 0` and `order`),
   remove it from its list, merge (take the lower index), increment `order`.
3. Repeat until `MAX_BUDDY_ORDER` or the buddy is not free.
4. Push the final merged block onto `buddy_lists[order]`.

All operations are protected by `buddy_lock` (spinlock).

---

## Public API

### Allocation

```c
// Allocate 2^ceil(log2(n)) contiguous pages for kernel use.
// Returns starting page index, or PHYMM_INVALID on failure.
unsigned phymm_alloc_kernel(unsigned page_count);

// Allocate a single page for user space.
unsigned phymm_alloc_user(void);
```

`phymm_alloc_kernel` rounds `page_count` up to the next power of two via
`ceil_log2`, then calls `buddy_alloc(order)`.
`phymm_alloc_user` is a thin wrapper for `buddy_alloc(0)` (order 0 = 1 page).

### Free

```c
void phymm_free_kernel(unsigned page_index, unsigned page_count);
void phymm_free_user(unsigned page_index);
```

`phymm_free_kernel` recovers the order from `phymm_pages[page_index].order`
(stored at allocation time); `page_count` is used as a fallback if the
stored order is out of range.

### Address conversion

```c
VIRT_TO_PHY(vaddr)          // virtual → physical: vaddr - KERNEL_OFFSET
PHY_TO_VIRT(paddr)          // physical → virtual: paddr + KERNEL_OFFSET
PHY_TO_PAGE_IDX(paddr)      // physical address → page index
VIRT_TO_PAGE_IDX(vaddr)     // virtual address → page index
```

---

## Reference counting (CoW / sharing)

Each physical page carries a reference count (`ref_count`) used to implement
copy-on-write and shared mappings:

```c
// Increment ref_count; increments phymm_used on first reference.
unsigned phymm_reference_page(unsigned page_index);

// Decrement ref_count; decrements phymm_used when it reaches zero.
unsigned phymm_dereference_page(unsigned page_index);

// True if ref_count > 1 (shared / CoW page).
int phymm_is_cow(unsigned page_index);

// True if ref_count > 0 (page is in use).
int phymm_is_used(unsigned page_index);
```

Both use `__sync_add_and_fetch` for atomic increment/decrement.

The page fault handler calls `phymm_is_cow()` to decide whether a write
fault should trigger a copy (allocate a new page, copy contents, update the
page table entry) or simply flip the PTE to writable.

---

## Dirty-page tracking

Used by file-backed `MAP_SHARED` mappings to track which pages must be
written back to disk on `munmap` or `msync`:

```c
void phymm_mark_dirty(unsigned page_index);   // set PHYMM_PAGE_DIRTY flag
int  phymm_is_dirty(unsigned page_index);
void phymm_clear_dirty(unsigned page_index);
```

---

## Boot-time initialisation sequence

Called from `boot_stage1()` after paging is enabled:

```
phymm_get_mgmt_pages(mem_high)
    └─ computes how many pages are needed to hold phymm_pages[]
       = ceil(total_page_count * sizeof(phymm_page) / PAGE_SIZE)

phymm_setup_mgmt_pages(start_page)
    └─ maps the management pages into the kernel virtual window
       (direct-mapped at start_page * PAGE_SIZE + KERNEL_OFFSET)
    └─ zeroes the phymm_pages[] array
    └─ sets phymm_pages pointer

phymm_init(mmap_addr, mmap_len)
    1. Initialises buddy_lists[] and buddy_lock
    2. Marks ALL pages in [phymm_begin, phymm_end) as PHYMM_RESERVED
    3. Walks the Multiboot memory map:
       - For each type-1 (usable RAM) region:
         - Clamps to [phymm_begin, phymm_end)
         - Clears ref_count (= 0, marks as free)
         - Adds range to buddy free lists via buddy_add_free_range()
```

`buddy_add_free_range(start, end)` populates free lists by greedily
assigning the largest aligned block that fits at each position, ensuring the
initial buddy lists are always power-of-two aligned.
