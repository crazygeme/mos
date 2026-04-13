# Virtual Memory

**Source:** `src/mm/mmap.c`, `src/mm/pagefault.c`, `src/mm/cache.c`, `src/syscall/syscall_sys.c`, `src/syscall/syscall_proc.c`, `src/ps/ps_fork.c`

## Status

MOS now has a Linux-like lazy virtual-memory layer:

- Per-process VM regions are tracked in a sorted tree with overlap-aware lookup.
- `mmap`, `munmap`, `mprotect`, and `brk` are implemented.
- Anonymous mappings fault in lazily.
- File-backed mappings fault in lazily from the filesystem page cache.
- `MAP_PRIVATE` writable pages use copy-on-write across `fork()`.
- `MAP_SHARED` file-backed pages are shared and written back on `munmap()` and process exit.
- `MAP_SHARED | MAP_ANONYMOUS` is supported via a kernel shared-page cache keyed by an internal `anon_id`.
- User stack growth is demand-driven: faults below the current stack bottom can extend the stack downward.

This document describes the behavior that exists in the current tree, not the older design.

## Address-Space Layout

The exact layout is defined by kernel constants, but the current split is:

- Low user addresses: ELF text/data/BSS and the `brk` heap.
- `TASK_UNMAPPED_BASE` and above: general `mmap()` area.
- Top of user space below `KERNEL_OFFSET`: grow-down user stack.
- `KERNEL_OFFSET` and above: kernel virtual address space.

`/proc/<pid>/maps` is populated from the VM region tree. Anonymous ranges are labeled as:

- `[heap]` when fully inside `start_brk..brk`
- `[stack]` when they match the task's current stack window

## VM Region Model

Each mapping is stored as:

```c
typedef struct _vm_region {
    unsigned begin;
    unsigned end;
    int prot;
    int flag;
    file *fp;
    int offset;
    unsigned anon_id;
} vm_region;
```

Important current behaviors:

- Regions are looked up with overlap semantics, so probing `[addr, addr + PAGE_SIZE)` finds the containing region.
- Insertion trims or splits any overlapping old region before inserting the new one.
- Adjacent compatible regions are coalesced automatically.
- Anonymous `MAP_SHARED` regions keep a non-zero `anon_id` so related mappings can find the same shared physical pages after `fork()`.
- `vm_find_vma()` can return the next higher VMA, not only an exact containing one; the page-fault path uses that for stack growth.
- A small per-task cache (`mmap_cache`) remembers the last VMA hit.

## `mmap`, `munmap`, `mprotect`, `brk`

### `mmap`

`do_mmap()` resolves the fd to a `file *` and delegates to `do_mmap_kernel()`.

Current semantics:

- `MAP_FIXED` replaces any overlapping mappings in the target range.
- Non-fixed mappings treat `addr` as a hint; if occupied, MOS falls back to the first free gap from `TASK_UNMAPPED_BASE`.
- Pages are not allocated at `mmap()` time.
- File mappings accept regular files and char devices.
- `fd == -1` is still treated as anonymous even if the caller omitted `MAP_ANONYMOUS`.

### `munmap`

`do_munmap()` iterates over every overlapping region and handles partial unmaps correctly:

- Dirty `MAP_SHARED` file-backed pages in the removed subrange are flushed first.
- Only the intersecting hardware PTEs are unmapped.
- Left and right remnants are reinserted as independent regions.

### `mprotect`

`sys_mprotect()`:

- requires page-aligned `addr`
- splits VM regions at the protection boundaries
- updates already-present PTE writability in place
- leaves not-yet-faulted pages to pick up the new protection later through the VM metadata

Current implementation only changes writable vs read-only at the PTE level. `PROT_NONE` is enforced in the page-fault path by treating the region as inaccessible.

### `brk`

`sys_brk()` grows and shrinks the heap between `start_brk` and `USER_HEAP_END`:

- growth maps additional heap pages with `MAP_FIXED`
- shrink unmaps whole pages above the new break
- `task->user->brk` may sit in the middle of the last mapped page, matching normal Unix `brk` behavior

## Page-Fault Handling

`pf_process()` distinguishes:

- non-present faults: demand allocation or file fault
- write faults on present read-only pages: COW or permission upgrade
- anything else: `SIGSEGV` in user mode, `DIE()` in kernel mode

The kernel exports page-fault counters through `/proc/mos`, including:

- `PfInvalid`
- `PfFile`
- `PfCow`
- `PfPerm`
- cache-hit and timing counters for file faults

## Anonymous Faults

### Private anonymous

- Writable private anonymous pages allocate a fresh user page on first fault and zero-fill it.
- Read-only private anonymous pages map a global shared zero page read-only.
- A later write to that zero page goes through normal COW.

### Shared anonymous

For `MAP_SHARED | MAP_ANONYMOUS`, faults use the anonymous shared-page cache in `src/mm/cache.c`:

- first fault on `(anon_id, page_offset)` allocates and zero-fills a page
- later faults in the same shared mapping find and reuse that page
- pages are mapped read-only first; a write fault upgrades the PTE in place because the mapping is shared

The shared cache is reference-counted by mapping lifetime, so the backing pages are released when the last related region disappears.

## File-Backed Faults

File faults are lazy and page-granular.

### Cache layers involved

There are three distinct cache-like structures in the current implementation, and they have different jobs:

1. Filesystem page cache in `src/fs/cache.c`
2. MM file-shared cache in `src/mm/cache.c`
3. MM anonymous-shared cache in `src/mm/cache.c`

They are easy to confuse, so it helps to think of them this way:

- the filesystem page cache is the primary cache for inode-backed file contents
- the MM file-shared cache is a fallback sharing table for mappings that do not participate in the fs page cache
- the MM anonymous-shared cache exists only for `MAP_SHARED | MAP_ANONYMOUS`

### Normal files

Current behavior:

- If the inode participates in the filesystem page cache, faults come from `fs_page_cache_get()`.
- Otherwise MOS can fall back to a direct page read path.
- `MAP_SHARED` file pages are additionally tracked in a file-shared cache so multiple mappings can reuse the same physical page.
- `MAP_PRIVATE` and read-only mappings are installed read-only first; the write path decides whether to COW.

Readonly private file mappings also get simple read-ahead via `pf_prefetch_pages()`.

### Filesystem page cache

The filesystem page cache is keyed by:

```c
typedef struct _fs_page_cache_key {
    void *tag;
    uint64_t ino;
    unsigned offset;
} fs_page_cache_key;
```

Meaning of the fields:

- `tag`: a stable address-space identity from `inode->i_pgcache_tag`
- `ino`: inode number
- `offset`: page-aligned file offset

An inode is eligible only when all of these are true:

- `inode != NULL`
- `inode->i_pgcache_tag != NULL`
- `inode->i_op->read_page` exists

So this cache is not "all files everywhere"; it is only for backends that provide a stable page-cache identity plus page-granular read support.

Current mechanics:

- lookup is protected by `fs_page_cache_lock`
- hits move the entry to the tail of an LRU list
- misses allocate a user physical page, temporarily kernel-map it, and fill it with `inode->i_op->read_page`
- the inserted cache entry takes an extra physical-page reference with `phymm_reference_page`
- eviction is LRU and bounded by `PAGE_CACHE_SIZE`

On concurrent misses, the code double-checks after I/O:

- if another thread inserted the page first, the newly loaded page is freed and the existing cached page is reused

Invalidation:

- `fs_page_cache_invalidate(inode)` drops every cached page matching the inode's `(i_pgcache_tag, i_ino)`
- ext4 and tmpfs writes/truncates call this so buffered readers and mmap faults do not keep stale file data

The important consequence for mmap is:

- when an inode has this cache, both buffered file I/O and page faults converge on the same physical cached file pages

### MM file-shared cache

The MM file-shared cache is keyed by:

```c
typedef struct _file_shared_map_key {
    void *tag;
    uint64_t ino;
    unsigned offset;
} file_shared_map_key;
```

It intentionally uses the same file identity idea as the fs page cache:

- `inode->i_pgcache_tag` when available
- otherwise `inode->i_private`
- plus `inode->i_ino`
- plus page-aligned file offset

This cache is used only from the page-fault path, and only when the inode does not go through the fs page cache.

Current flow in `pf_get_file_page()`:

- if the file uses the fs page cache, use `fs_page_cache_get()` and stop there
- else, for `MAP_SHARED`, probe `mm_file_shared_find(f, offset)`
- on miss, read the page directly from the file backend
- if that miss was for `MAP_SHARED`, register it with `mm_file_shared_add()`

So the MM file-shared cache is a fallback sharing registry for file-backed mappings that still need `MAP_SHARED` semantics but are outside the fs page-cache path.

Like the other shared maps in `src/mm/cache.c`, inserting an entry pins the physical page with an extra reference count.

Notably, there is no standalone invalidation path for this cache today that mirrors `fs_page_cache_invalidate()`. The implementation relies on stable backing identity and mapping lifetime rather than a richer coherent address-space object.

### `/dev/mem`

`/dev/mem` is treated specially:

- the fault maps the requested physical page directly
- the PTE is cache-disabled
- no filesystem-page-cache semantics are involved

This is required for MMIO and framebuffer mappings.

## Write Faults and `fork()`

`fork()` duplicates the VM region tree, then copies present PTEs VMA-by-VMA.

Current rule set:

- `MAP_PRIVATE` + writable present page: parent and child are both write-protected, so the first writer faults and gets a private copy
- `MAP_SHARED` or read-only present page: child receives the same PTE without forced COW
- `/dev/mem` and other non-managed physical pages are cloned as raw mappings without feeding them into RAM refcount logic

Write faults use:

- `wp_page_copy()` when the page is still shared
- `wp_page_reuse()` when this task is already the sole owner, or when a `MAP_SHARED` page only needs its writable bit restored

For shared file mappings, the first successful write fault also marks the backing physical page dirty.

## Dirty Writeback

Dirty writeback currently exists for `MAP_SHARED` file-backed mappings only.

`vm_flush_dirty_region()`:

- skips anonymous mappings
- skips `/dev/mem`
- skips never-faulted pages
- writes back only pages whose physical page is marked dirty

Flush points in the current tree:

- before unmapping a shared file-backed subrange in `do_munmap()`
- on overlap replacement inside `vm_add_map()`
- on process teardown through `vm_flush_all_dirty()`
- for one file identity through `vm_flush_file_dirty()`

There is no implemented `msync(2)` syscall entry yet; syscall slot 144 is still empty.

## Shared-Page Caches In `mm/cache.c`

`src/mm/cache.c` owns two MM-side sharing registries:

- `anon_shared_map`
- `file_shared_map`

### Anonymous shared map

The anonymous shared map is keyed by:

```c
typedef struct _anon_shared_map_key {
    unsigned id;
    unsigned offset;
} anon_shared_map_key;
```

The stored `id` is:

- `anon_id | ANON_SHARED_BIT`

The high-bit tag keeps anonymous-shared identities separate from ordinary numeric ids and makes the map key unambiguous inside the MM layer.

The lifetime model has two pieces:

- `anon_shared_map`: `(anon_id, offset) -> phy`
- `anon_shared_refs`: `anon_id -> number of live VM regions still referring to that shared object`

Why both exist:

- the page map tells faults which physical page backs a given shared-anon offset
- the ref table tells the kernel when the last related VMA has disappeared and the whole shared-anon object can be dropped

Current lifetime rules:

- `vm_add_map()` calls `mm_anon_shared_get(anon_id)` when inserting a shared-anon region
- region destruction or replacement eventually calls `mm_anon_shared_put(anon_id)`
- when the refcount falls to zero, every cached page for that `anon_id` is removed from `anon_shared_map`
- each removed page drops its extra physical-page reference, freeing the page if nothing else still maps it

### File shared map

The file shared map is much simpler:

- there is no parallel per-object ref table
- entries live as long as the MM shared registry retains them
- insertion takes an extra page reference

Its purpose is only to preserve `MAP_SHARED` page sharing for non-fs-page-cache backends.

## Fault Walkthroughs

### File-backed `MAP_SHARED` with fs page cache

1. Fault on file offset `X`.
2. `pf_file_uses_fs_page_cache()` says yes.
3. `fs_page_cache_get(inode, X)` returns the shared physical page for `(tag, ino, X)`.
4. The process maps that page.
5. Write faults upgrade the PTE in place and mark the page dirty.
6. `munmap()` or exit flushes dirty pages back through `write_page` or the ext4 fallback path.

### File-backed `MAP_SHARED` without fs page cache

1. Fault on file offset `X`.
2. `pf_file_uses_fs_page_cache()` says no.
3. `mm_file_shared_find(file, X)` is checked.
4. On hit, the existing shared physical page is reused.
5. On miss, the page is read directly from the backend and inserted into `file_shared_map`.

### Anonymous `MAP_SHARED`

1. `do_mmap_kernel()` assigns a fresh `anon_id`.
2. `fork()` preserves that `anon_id` in child VM regions.
3. Fault on offset `X` checks `mm_anon_shared_find(anon_id, X)`.
4. On miss, a zero-filled page is allocated and registered.
5. Later faults in parent/child/shared remnant VMAs find the same physical page.

## Stack Growth

The user stack is represented as an anonymous grow-down mapping near the top of user space.

On a fault:

- `pf_find_vma()` asks for the first VMA at or above the address
- if the next VMA is the current stack mapping and the fault is within the allowed grow-down window, MOS extends the stack downward with `MAP_FIXED`
- the new pages still fault in lazily afterward

Growth happens in at least `USER_STACK_INIT_PAGES` chunks, not strictly one page at a time.

## Limitations and Notes

- `mprotect()` currently only changes writable state in hardware PTEs; execute permissions are tracked in metadata but not enforced by x86 NX.
- `PROT_NONE` is enforced by fault handling rather than by installing a special non-accessible PTE.
- Shared anonymous pages and shared file pages are cached in kernel-global structures; those caches are simple and correctness-oriented, not aggressively optimized.
- Dirty shared-file writeback exists, but `msync()` is not wired up yet.
- VM enumeration order depends on the tree traversal used by `hash_first/hash_next`; the implementation expects it to be sorted by address.
