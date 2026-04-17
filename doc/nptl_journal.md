# NPTL Journal

A focused log of the NPTL-related debugging and fixes done on 2026-04-17.
This document is narrower than `bugfix_journal.md`: it records the `clone()`,
`CLONE_CHILD_SETTID`, `script.posix_signal`, and `startx`/VDSO work from this
session.

---

## 2026-04-17 - `script.posix_signal` hung in `nptl`

**Symptom**: `script.posix_signal` blocked at the final background-subshell
case on the `nptl` branch, while the non-`nptl` branch did not.

**Observed log pattern**:

- parent bash did `clone(flags=1200011, ..., ctid=4002b2c8)`
- parent immediately did `kill(child, SIGTERM)` and then `wait()`
- the child first appeared only at the end of the log as
  `sig_deliver(15)` followed by `rt_sigprocmask(0)`

That showed the child was still taking `SIGTERM` on the caught-handler path,
not the default-terminate path.

### Userspace analysis

This was not guessed from shell behavior alone. Bash 2.05b source was fetched
and checked in `.codex-data/context/bash-2.05b`.

The failing script pattern is:

```sh
(
    trap 'exit 0' TERM
    trap - TERM
    sleep 30
) &
kill -TERM $child
wait $child
```

Bash has an explicit historical comment for this race: the parent may signal
the background subshell before the subshell has run the code that restores
signals to their original dispositions.

### Root cause

The `nptl` branch uses process-style `clone()` with:

- `SIGCHLD`
- `CLONE_CHILD_SETTID`
- `CLONE_CHILD_CLEARTID`

MOS returned to the parent quickly enough that the parent could signal the new
child before the child ran its first fork-return path and before bash executed
its `trap - TERM`.

So the kernel bug was not in generic signal delivery. The kernel-side problem
was the scheduling/first-run behavior of non-thread `clone(SIGCHLD...)`.

### Fix

In [`src/ps/ps_clone.c`](../src/ps/ps_clone.c):

- process-style `clone()` now uses a dedicated enqueue helper
- the new child is inserted at the head of its ready queue for its first run
- the parent yields once with `task_sched()` after enqueueing

This gives the newborn process one early run before the parent continues far
enough to signal it.

### Why the first yield-only fix was insufficient

An intermediate patch added only `task_sched()` after enqueueing. The log still
showed pid `3` running straight through `kill(11, 15)` before pid `11` made any
normal syscall. That meant the scheduler was choosing the parent again. The
final fix therefore changed both:

- enqueue position
- post-enqueue yield

---

## 2026-04-17 - `CLONE_CHILD_SETTID` for process-style `clone()` wrote into the wrong address space

**Symptom**: the `nptl` branch exercises glibc's process-style `clone()` path,
but MOS originally handled `CLONE_CHILD_SETTID` like:

```c
*child_tidptr = task->psid;
```

That is only correct for shared-VM thread clones. For non-`CLONE_VM` children
it writes through the parent's address space, not the child's.

**Root cause**: MOS treated `CLONE_CHILD_SETTID` as if the child pointer always
belonged to the current address space. In glibc/NPTL's fork-like `clone()`
case, the pointer is meaningful in the child address space.

### Final implementation

In [`src/ps/ps.c`](../src/ps/ps.c), added:

```c
int ps_write_process_memory(task_struct *task, void *addr,
                            const void *src, unsigned len);
```

In [`src/ps/ps_clone.c`](../src/ps/ps_clone.c):

- thread-group/shared-VM clones still write `*child_tidptr` directly
- process-style clones now call `ps_write_process_memory(...)`

### What the helper does

`ps_write_process_memory()` walks the target task's page directory directly and
writes into the child's virtual address space before the child runs.

For a writable mapped page:

- it resolves the child's PTE
- writes directly to the backing page at the requested offset

For a read-only COW page:

- it allocates a private page for the child
- copies the original page contents
- applies the requested write into the new page
- installs the new writable PTE in the child
- drops the old page reference

This made `CLONE_CHILD_SETTID` work for the `nptl` process-style `clone()` ABI.

---

## 2026-04-17 - Rejected refactor: generic cross-process write helper caused regressions

An intermediate attempt generalized the child-memory write logic into a
process-wide helper (`ps_write_process` / `ps_write_task_memory`) and moved too
much page-table/COW machinery into the generic `ps` layer.

That version triggered new failures:

- userspace bash segfaulted (`eip = 0`)
- later kernel cleanup paths faulted inside list handling

### Bugs found during that failed refactor

1. A newly allocated private page was installed into the child PTE without
   taking the allocator reference, so it could be recycled while still mapped.
2. The generic path was too invasive for code that is really specific to the
   pre-first-run `clone()` child-tid case.

### Resolution

That refactor was backed out. The final code keeps the generic helper only as
`ps_write_process_memory(task, ...)`, which is still specialized around the
task-level pre-start write use case and is only consumed by `clone()`.

---

## 2026-04-17 - `startx` regressed in `nptl` because the VDSO lived inside XFree86 int10 low memory

**Symptom**: `startx` worked on the non-`nptl` branch but failed again on the
`nptl` branch. The X server stopped at:

```text
(II) VESA(0): initializing int10
```

and then exited. In `krn.log`, the matching path was:

- X mapped `/dev/mem` at `0xf0000`, `0xa0000`, and `0xc0000`
- X created the low-memory SysV SHM segment
- X attached that segment twice:
  - once at an arbitrary base
  - once at virtual address `0x00000000` via `shmat(..., 1, SHM_RND)`
- the process then exited before logging:
  - `Primary V_BIOS segment is: 0xc000`
  - `VESA BIOS detected`

Those two lines do appear in the successful run captured in
`/var/log/XFree86.1.log`, so the regression was before the first successful
VBIOS detection.

### Root cause

The `nptl` branch introduced a live VDSO mapping and placed it at:

```c
0x00010000
```

That address is inside the legacy low-memory range that XFree86 int10 remaps
with:

```c
shmat(low_mem, (char*)1, SHM_RND)
```

which rounds down and maps the SysV SHM segment over `0x00000000-0x0009ffff`.

So on the `nptl` branch, XFree86's low-memory int10 attach could overwrite the
user-visible VDSO page and its VMA. The non-`nptl` branch did not have a real
live VDSO fast-syscall mapping there, which explains why it did not hit the
same regression.

### Fix

In [`src/mm/vdso.c`](../src/mm/vdso.c) and
[`include/mm/vdso.h`](../include/mm/vdso.h):

- removed the fixed low-memory VDSO address
- made the VDSO base dynamic
- mapped it just below `USER_ZONE_END`, i.e. at the top of normal user mmap
  space and far away from the first 640KB used by XFree86 int10

`AT_SYSINFO` now points at that relocated high mapping instead of the old
`0x00010000` region.

### Why this was an `nptl`-only bug

The non-`nptl` branch effectively had no active VDSO fast-syscall code, so
overwriting low memory during XFree86 int10 setup did not destroy anything
important. The `nptl` branch did, and the low VDSO placement made the new
mapping collide with software that still expects to own legacy low memory.

---

## Files changed in the final working version

- [`src/ps/ps_clone.c`](../src/ps/ps_clone.c)
  - process-style `clone()` child-first enqueue path
  - one-time post-enqueue yield
  - correct `CLONE_CHILD_SETTID` handling for non-`CLONE_VM`
- [`src/ps/ps.c`](../src/ps/ps.c)
  - `ps_write_process_memory(...)`
- [`src/ps/ps_internal.h`](../src/ps/ps_internal.h)
  - internal declaration for `ps_write_process_memory(...)`
- [`src/mm/vdso.c`](../src/mm/vdso.c)
  - VDSO relocated out of low memory into high user mmap space
- [`include/mm/vdso.h`](../include/mm/vdso.h)
  - removed the fixed `0x00010000` VDSO address

---

## Net result

Today fixed three distinct `nptl` issues:

1. `CLONE_CHILD_SETTID` now targets the child address space correctly for
   process-style `clone()`.
2. The final `script.posix_signal` race no longer hangs because the newborn
   process-style clone gets its first run before the parent races ahead and
   signals it.
3. `startx` works again because the `nptl` VDSO no longer collides with
   XFree86 int10's low-memory `shmat()` mapping.
