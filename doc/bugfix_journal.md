# Bug Fix Journal

A running log of non-obvious bugs, their symptoms, root causes, and fixes.
Each entry explains the reasoning so the same mistake is not repeated.

---

## 2026-04-10 — SSH `vim` burst, SSH `exit` hang, and nonblocking IPC semantics

This round started with `vim` disconnecting over SSH and ended up exposing two
separate POSIX-visible bugs: stream sockets treated temporary TCP backpressure
as a hard write failure, and nonblocking anonymous pipes/PTYs reported EOF too
eagerly instead of `EAGAIN`. The visible symptoms looked unrelated at first,
but both surfaced through OpenSSH.

### 1. `vim` over SSH failed on the first full-screen redraw

- **Caught by:** manual SSH session using `vim`
- **Symptom:** interactive shell traffic worked, but starting `vim` caused the
  SSH session to drop during the first large terminal repaint.
- **Root cause:** the TCP stream send path tried to enqueue one large write at
  a time and treated lwIP `ERR_MEM` as a fatal send error. Under screen-sized
  SSH packets, `tcp_write()` could temporarily reject the request because the
  send buffer had less space than the whole payload.
- **Fix:** in [sock.c](/home/zhengjia/project/mos/src/net/sock.c) and
  [sock_msg.c](/home/zhengjia/project/mos/src/net/sock_msg.c), split TCP stream
  writes into `tcp_sndbuf()`-sized chunks and retry blocking sends until space
  is available instead of failing immediately. Keep the `tcp_sent` wakeup path
  in `sock_cb.c` so blocked writers resume once ACKs free buffer space.

### 2. SSH `exit` hung because OpenSSH's self-pipe saw false EOF

- **Caught by:** manual SSH session exiting the remote shell
- **Symptom:** the shell exited, but the SSH connection stayed open and the
  server spun inside OpenSSH's `notify_done()` loop.
- **Root cause:** MOS anonymous pipe reads returned `0` for an empty
  nonblocking pipe even while writers were still open. OpenSSH drains its
  SIGCHLD self-pipe with `while (read(...) != -1)`, so receiving `0` instead of
  `-EAGAIN` made it loop forever on what looked like EOF.
- **Fix:** in [pipe.c](/home/zhengjia/project/mos/src/fs/pipe.c), make
  nonblocking reads return `-EAGAIN` when the pipe is empty but has live
  writers, and reserve `0` for true EOF only. Also update pipe poll readiness
  to report readable on buffered data or real EOF.

### 3. PTYs had the same nonblocking-read mismatch

- **Caught by:** follow-up review after fixing anonymous pipes
- **Symptom:** PTY reads ignored `O_NONBLOCK`, and empty PTYs could not cleanly
  distinguish "try again later" from real EOF.
- **Root cause:** PTY master/slave read paths always called `cyb_getbuf(..., 1,
  1)`, which forced blocking behavior and collapsed nonblocking semantics.
- **Fix:** in [pts.c](/home/zhengjia/project/mos/src/dev/pts.c), teach both
  master and slave read paths to honor `O_NONBLOCK`, return `-EAGAIN` on empty
  PTYs with a live peer, and preserve `0` for peer-close EOF. PTY poll
  readiness was also updated to treat EOF as readable.

### 4. `sendmsg()` / `recvmsg()` had duplicated logic across AF_INET and AF_UNIX

- **Caught by:** refactor after the bug fixes stabilized
- **Symptom:** the generic and AF_UNIX message paths each had their own copies
  of iovec sizing, scatter/gather ring-buffer copies, `MSG_DONTWAIT` checks,
  and cmsg append logic, which made the bug fixes easy to apply inconsistently.
- **Root cause:** message-socket helpers had grown independently in
  `sock_msg.c` and `sock_un.c`.
- **Fix:** factor shared helpers into the socket layer:
  - in [sock.h](/home/zhengjia/project/mos/include/net/sock.h) and
    [sock.c](/home/zhengjia/project/mos/src/net/sock.c), add `rx_iov_write()`,
    `rx_iov_read()`, and `rx_discard()`
  - in [sock_msg.c](/home/zhengjia/project/mos/src/net/sock_msg.c), expose
    shared helpers for iovec total length, nonblocking flag handling, and cmsg
    append
  - in [sock_un.c](/home/zhengjia/project/mos/src/net/sock_un.c), reuse those
    helpers instead of maintaining local duplicates

### 5. Regression coverage was missing for this exact class of bug

- **Caught by:** the need to make the SSH fixes stick
- **Symptom:** nothing in the script suite directly checked the crucial
  `-EAGAIN` versus EOF distinction for nonblocking pipes and PTYs.
- **Root cause:** existing tests covered basic pipes and PTYs, but not the
  OpenSSH-triggering edge case where an empty nonblocking endpoint must not
  look like EOF while the writer side is still alive.
- **Fix:** add [posix_nonblock_ipc.sh](/home/zhengjia/project/mos/test/posix_nonblock_ipc.sh),
  which verifies:
  - anonymous pipe nonblocking reads return `-EAGAIN` while the writer is open
  - anonymous pipe reads return `0` only after writer close
  - PTY master/slave reads follow the same rule

### Result

- Manual verification:
  - `vim` now works over SSH
  - remote `exit` now closes the SSH session correctly
- Regression verification:
  - `./run.sh kvm test logtofile`
  - result: `rc=0`

---

## 2026-04-10 — POSIX script-suite fixes from the long `./run.sh test` loop

This round started as a hang in `posix_wait.sh` and ended with a full green
run of the `/proc/tests/all_script` suite. The failures were mostly not hard
crashes; they were quiet POSIX-visible mismatches that only became obvious once
 the script suite was run continuously.

### 1. `waitpid()` and `sleep 0` combined into a false hang

- **Caught by:** `test/posix_wait.sh`
- **Symptom:** the suite could get stuck forever in the wait test.
- **Root cause:** two issues compounded:
  - `waitpid(..., WNOHANG)` treated `WNOHANG` like an exact option value
    instead of a bit flag
  - `nanosleep()` with zero duration blocked instead of returning immediately
- **Fix:** in `src/ps/ps_syscall.c`, make `WNOHANG` checks use
  `options & WNOHANG` and replace stale child-count checks with live scans of
  the parent/child tree; in `src/syscall/syscall_sys.c`, make zero-length
  `nanosleep()` return immediately.

### 2. Unlinking an open file broke active file descriptors

- **Caught by:** `test/posix_fcntl.sh`, `test/posix_proc.sh`,
  `test/posix_fd_pass.sh`, and related shell heredoc/temp-file use
- **Symptom:** userspace saw `Input/output error` when a temp file was unlinked
  while still open.
- **Root cause:** the ext4-backed path removed the live file immediately even
  when processes still held open descriptors to it.
- **Fix:** add delayed unlink semantics:
  - add `f_state` plus `FS_FILE_UNLINK_ON_CLOSE` in `include/fs/fs.h`
  - in `src/syscall/syscall_fs.c`, rename open unlinked files to a hidden
    tombstone and retarget all matching open file objects
  - in `src/fs/root.c`, remove the tombstone during final file release

### 3. Append, positional I/O, and inode size tracking were incomplete

- **Caught by:** `test/posix_fcntl.sh`
- **Symptom:** append writes could overwrite existing bytes, and positional I/O
  could disturb the live file cursor or backend cursor.
- **Root cause:** `pread()` / `pwrite()` used the file position incorrectly,
  and ext4 writes did not fully honor `O_APPEND` or refresh inode size.
- **Fix:** in `src/fs/fs.c`, make `pread()` and `pwrite()` operate on a
  temporary position and restore the underlying seek position; in
  `src/fs/root.c`, make append writes use EOF and update inode size on growth.

### 4. `RLIMIT_FSIZE`, `umask`, and `creat()` semantics were too weak

- **Caught by:** `test/posix_rlimit.sh` and `test/posix_umask.sh`
- **Symptom:**
  - writes past `RLIMIT_FSIZE` succeeded instead of failing with `EFBIG`
  - files and directories ignored the process umask on creation
  - `creat()` bypassed the normal open/create path
- **Root cause:** those paths were either stubs or incomplete shortcuts.
- **Fix:**
  - in `src/fs/fs.c`, enforce `RLIMIT_FSIZE` in write paths
  - in `src/syscall/syscall_proc.c`, implement `getrlimit()` by delegating to
    `ugetrlimit()`
  - in `src/fs/fs.c` and `src/syscall/syscall_fs.c`, apply umask to newly
    created files and directories
  - in `src/syscall/syscall_fs.c`, route `creat()` through the normal
    `fs_open(..., O_CREAT | O_TRUNC, mode)` path
  - in `src/fs/root.c`, explicitly fix ext4 directory mode after creation

### 5. Legacy `signal(2)` and self-signal delivery were missing edge behavior

- **Caught by:** `test/posix_signal.sh`
- **Symptom:** shell trap/self-signal cases did not behave like normal Unix
  signal delivery.
- **Root cause:**
  - syscall 48 (`signal`) was still unimplemented
  - self-directed `kill(getpid(), sig)` did not reliably deliver soon enough
    for the shell trap expectations used by the test harness
- **Fix:**
  - implement `signal(2)` in `src/ps/ps_signal.c` and register it in
    `src/syscall/syscall.c`
  - for self-directed unmasked signals, trigger delivery immediately on the
    current user return frame in `src/ps/ps_signal.c`
  - update `test/posix_signal.sh` to signal the current shell via a helper
    process using `PPID`, which is stable under the wrapper script model

### 6. Broken pipes returned `EPIPE` but did not raise `SIGPIPE`

- **Caught by:** `test/posix_pipe.sh`
- **Symptom:** shell pipelines such as `cat file | head -n 1` failed even
  though the pipe buffer code detected the broken-pipe condition.
- **Root cause:** pipe writes returned `-EPIPE` but never queued `SIGPIPE` for
  the writer.
- **Fix:** in `src/fs/pipe.c`, when `cyb_putbuf()` reports `-EPIPE`, queue
  `SIGPIPE` for the current user task.

### 7. A couple of test scripts were asserting the wrong shell semantics

- **Caught by:** `test/posix_environ.sh` and `test/posix_signal.sh`
- **Symptom:** some failures were in the tests themselves rather than the
  kernel.
- **Root cause:**
  - `${EMPTY_EXPORT:-notset}` treats an empty variable as defaulted, so it was
    not a valid check for "exported but empty"
  - `$$` under the wrapped script execution model was not the right stable way
    to target the shell process under test
- **Fix:**
  - in `test/posix_environ.sh`, check the actual variable value directly
  - in `test/posix_signal.sh`, use a helper shell that signals `PPID`

### Result

- Final verification command:
  - `./run.sh test curses logtofile`
- Final result:
  - `Total 40 Cases, 0 Failed`

---

## 2026-04-07 — Page-fault VMA lookup now uses a Linux-style `mmap_cache`

### Symptom

- The old page-fault path did a full VM-tree lookup for every fault and mixed
  two unrelated ideas under the same name: a file-page cache in
  `src/mm/pagefault.c` and Linux's actual `mmap_cache`, which is a per-task
  cache of the last `find_vma()` result.

### Root cause

- MOS had no Linux-style VMA cache at all, so repeated faults in the same VMA
  kept rewalking the interval tree.
- Stack growth logic was separate from VMA lookup instead of following the
  Linux pattern: `find_vma()`, then if the fault lands in a hole immediately
  below the grow-down stack VMA, expand that VMA and retry.
- The old in-file "mmap cache" name made the code harder to reason about,
  because it was really a custom readonly private-file page cache rather than
  a VMA cache.

### Fix

- Add `user->mmap_cache` as a Linux-style cache of the last `find_vma()`
  result.
- Add `vm_find_vma()` plus cached helpers so the fault path can distinguish
  "address is inside this VMA" from "address is in a hole but this is the next
  VMA above it".
- Rework `src/mm/pagefault.c` to use that flow for demand faults and stack
  growth.
- Remove the custom readonly `MAP_PRIVATE` file-page cache from the page-fault
  layer and move file-backed caching down into the filesystem page-cache path,
  so `read()` and mmap faults share cached file pages.
- Keep that shared file-backed cache in `src/fs/cache.c`, separate from the
  generic fs descriptor code in `src/fs/fs.c`.
- Invalidate the per-task VMA cache on `mmap`, `munmap`, `mprotect`, `fork`,
  and `exec`.

### Why this is better

- It matches the real Linux mechanism more closely, so the fault path is easier
  to compare against upstream behaviour.
- It removes both the naming collision and the extra fault-layer cache that was
  hiding the real design issue.
- It gives repeated faults in the same mapping a fast path without changing the
  existing VMA lookup, on-demand paging, and COW model, while also making file
  reads and mmap faults converge on one cache.

---

## 2026-04-07 — Shared-mapping cleanup after the boot/MM fixes

### Symptom

- After the boot fix, anonymous `MAP_SHARED` pages still lived forever once
  faulted in, and the emergency file-backed `MAP_SHARED` fallback in
  `src/mm/pagefault.c` was broader than intended.

### Root cause

- `anon_shared_map` had no lifetime tracking tied to VM-region teardown, so
  `munmap`, VMA splits/merges, and process exit never reclaimed the cached
  pages when the last sharer disappeared.
- The file-backed fallback path would also activate for files that already had
  a proper filesystem page-cache identity, even though those faults should be
  satisfied by the fs page cache or fail cleanly.

### Fix

- Add `mm_anon_shared_get()` / `mm_anon_shared_put()` and a small
  `anon_shared_refs` table in `src/mm/cache.c`.
- Wire those refs through `vm_add_map`, `vm_region_invalid`, `vm_mprotect`,
  `do_munmap`, and VMA coalescing so shared anonymous objects survive transient
  descriptor surgery but are reclaimed when the last live mapping goes away.
- Narrow the MM-cache-layer `file_shared_map` fallback so it is only used for
  `MAP_SHARED` files that cannot participate in the filesystem page cache.

### Why this is better

- Shared anonymous pages now have a real lifetime tied to live VM mappings
  instead of kernel uptime.
- The file fault path is closer to Linux: use the filesystem page cache when
  available, and keep the local fallback strictly as a compatibility escape
  hatch rather than a shadow cache.

---

## 2026-04-07 — Generic block-device lookup for ext mounts

### Symptom

- `ext4_get_sb()` in `src/fs/root.c` knew too much about storage internals: it
  stripped `/dev/` by hand, walked `hdd_partitions`, checked loop-device state
  directly, and special-cased root-device selection with `hdd_partitions[0]`.

### Root cause

- MOS had no generic block-device registry, so the ext mount path had to know
  where HDD partitions and loop devices each stored their bookkeeping.
- The public HDD partition array leaked hardware-driver internals into the fs,
  exec, proc, and `/dev` layers.

### Fix

- Add a small generic block-device registry in `src/dev/blockdev.c`.
- Register discovered HDD partitions and loop slots there, marking only
  attached/usable devices as mountable.
- Switch `ext4_get_sb()`, root mount, and exec-time remount to resolve devices
  through that registry instead of peeking at `hdd_partitions` / `loop_devs`.
- Make the HDD partition table private to `src/hw/hdd.c` and expose only
  accessors used by `/dev/hdd` and `/proc/partitions`.

### Why this is better

- The ext mount path is closer to Linux: filesystems consume a generic block
  device abstraction instead of parsing driver-private tables.
- HDD bookkeeping is no longer exported as a global array.
- Future block devices can become mount sources by registering once, without
  editing ext4 mount code again.

---

## 2026-04-07 — Bugs flushed out by the new `/proc/tests` script suite

After moving guest tests into `test/*.sh` and wiring them into
`/proc/tests/all_script`, the expanded script coverage started finding a
different class of bugs: not just kernel panics, but quiet POSIX-visible
semantic mismatches.  This section is a compact summary of the issues the new
script suite exposed and the fixes that landed.

### 1. Existing-directory `mkdir` succeeded instead of failing

- **Caught by:** `test/ret_fs.sh`
- **Symptom:** `mkdir existing-dir` succeeded silently instead of failing with
  the usual `EEXIST` behaviour.
- **Root cause:** `lwext4` directory creation checked whether the path already
  existed, but returned success instead of `EEXIST`.
- **Fix:** Return `EEXIST` from `ext4_dir_mk()` for the already-existing case.

### 2. `mkdir` with a missing parent incorrectly succeeded

- **Caught by:** `test/posix_mkdir.sh`
- **Symptom:** creating `/a/missing/child` worked when the parent directory did
  not exist.
- **Root cause:** the root filesystem `mkdir` path delegated too early into the
  ext4 layer without first validating the parent directory.
- **Fix:** validate the parent in `src/fs/root.c` before issuing the create.

### 3. `rmdir` allowed non-empty directories

- **Caught by:** `test/posix_dirs.sh`
- **Symptom:** removing a non-empty directory succeeded.
- **Root cause:** the removal path did not enforce the standard
  `ENOTEMPTY` check before unlinking the directory entry.
- **Fix:** reject `rmdir()` on non-empty directories in the root filesystem.

### 4. `O_APPEND` opens did not actually append

- **Caught by:** `test/posix_io.sh`
- **Symptom:** writes to an `O_APPEND` descriptor could overwrite existing
  contents instead of always landing at EOF.
- **Root cause:** open/write handling did not consistently move the file
  position to the current end of file for append-mode descriptors.
- **Fix:** honor `O_APPEND` in the file path so append writes are serialized at
  EOF.

### 5. Re-mounting an image as a loop device could crash the kernel

- **Caught by:** `test/loopdev.sh` and `test/ret_mount.sh`
- **Symptom:** mounting the same image again, or exercising duplicate-mount
  paths, could crash partway through the guest test run.
- **Root cause:** loop-device and mount-state handling did not reject duplicate
  mounts cleanly and had unsafe assumptions in the remount path.
- **Fix:** make duplicate mounts return `-EBUSY`, tighten loop-device handling,
  and remove the crash path.

### 6. Renaming a directory into its own descendant was not rejected

- **Caught by:** `test/posix_rename.sh`
- **Symptom:** invalid rename patterns that should fail could corrupt the tree
  or behave unpredictably.
- **Root cause:** the rename path did not detect the ancestor/descendant case.
- **Fix:** reject attempts to move a directory into its own subtree.

### 7. `/dev/pts` names were generated in hexadecimal

- **Caught by:** `test/dev_pts.sh`
- **Symptom:** PTY paths under `/dev/pts` used hex-like names instead of the
  normal decimal numbering expected by userspace.
- **Root cause:** `src/dev/ptmx.c` formatted PTY indices with `%x`.
- **Fix:** emit PTY names with `%d` so `/dev/pts/0`, `/dev/pts/1`, ... match
  normal Unix behaviour.

### 8. Read-only mmap cache eviction leaked pages and later caused crashes

- **Caught by:** heavy userspace tests such as
  `test/dev_subsystem.sh`, `test/emacs_smoke.sh`, and `test/gcc_hello.sh`
- **Symptom:** long script runs would eventually die in unrelated places after
  enough exec/mmap churn.
- **Root cause:** cached file-backed pages were dereferenced on eviction but not
  physically freed when the last reference disappeared.
- **Fix:** free the physical pages when the readonly file-cache refcount drops
  to zero, and keep cache invalidation coherent on file changes.

### 9. `proc_buf_printf()` silently truncated large generated `/proc/tests` files

- **Caught by:** the generated script infrastructure itself
- **Symptom:** larger synthetic files such as the combined test wrappers could
  hang or produce incomplete content once formatting exceeded the old 512-byte
  scratch space.
- **Root cause:** `proc_buf_printf()` relied on a fixed-size temporary buffer
  and had no way to ask `vsprintf()` for the true output length first.
- **Fix:** teach `vsprintf(NULL, ...)` / `sprintf(NULL, ...)` to return the
  exact size, then grow `proc_buf_t` accordingly before formatting.

### 10. tmpfs had several quiet POSIX mismatches

- **Caught by:** `test/posix_tmpfs.sh`
- **Symptoms:**
  - `ftruncate()` shrink/regrow could expose stale bytes
  - sparse writes past EOF could leave uninitialized data in the gap
  - `chmod`, `chown`, and `utime` on tmpfs objects were incomplete
  - directory enumeration omitted `.` and `..`
  - `unlink(dir)` / `rmdir(dir)` semantics were wrong
- **Root cause:** the initial tmpfs implementation was a narrow `/dev/shm`
  vehicle and had not been brought up to normal POSIX-visible directory and
  metadata semantics.
- **Fix:** add proper zero-fill on shrink/regrow and sparse extension, implement
  metadata updates, report `.` / `..`, compute directory link counts more
  accurately, reject directory unlink, and make `rmdir()` remove directories
  through the correct path.

### Key Lesson

Guest shell-script coverage is excellent at finding the "boring" bugs that are
easy to miss during kernel bring-up: wrong errno values, stale data exposure
after truncate, malformed `/dev` naming, directory edge cases, and metadata
operations that quietly return success without doing the right thing.  These
are precisely the bugs that make a system feel non-POSIX even when the big
features already work.

---


## 2026-04-06 — OpenSSH 3.5p1 `sshd` crash / privilege-separation bring-up

### Symptoms

- Connecting to the guest with `ssh root@10.0.5.18` initially crashed the
  per-connection `sshd` child immediately after `fork()`.
- The kernel log showed the child resetting signal handlers with
  `rt_sigaction(...)` and then dying in libc.
- After the crash was fixed, `sshd` still rejected connections because
  privilege separation reached an unimplemented `chroot(2)`.

### Root Causes

#### 1. `select(2)` overflowed heap-allocated `fd_set` buffers

MOS `do_select()` copied and cleared a full `sizeof(fd_set)` regardless of the
caller's `nfds`.  OpenSSH 3.5p1 heap-allocates only the number of bytes needed
for the requested descriptor range.  The kernel therefore wrote past the end of
that heap buffer on every `pselect()`, corrupting malloc metadata and causing
later crashes inside glibc.

#### 2. `sys_brk(0)` was not query-only

On the first call, `sys_brk()` force-mapped one page and advanced `brk` even
when userspace only wanted to query the current break.  That is not Linux
behaviour and confuses allocators that expect `brk(0)` to be side-effect free.

#### 3. `rt_sigaction` used the wrong userspace layout

glibc 2.3.2 on Linux/i386 uses a `kernel_sigaction` layout with a full
1024-bit userspace `sigset_t` in memory.  MOS had modeled the structure as only
two words of mask storage, so the kernel and glibc disagreed about the stack
object layout.

#### 4. Privilege separation required `chroot(2)` plus jail-aware path lookup

After the memory corruption issues were fixed, OpenSSH reached its privsep
preauth child and attempted to:

```text
chroot(/var/empty/sshd)
chdir(/)
setgid(...)
setuid(...)
```

`__NR_chroot` was unimplemented, and path resolution only understood the
process CWD, not a per-process jailed root.

### Fixes

**`src/fs/select.c`**

- Reject `nfds > FD_SETSIZE`
- Size all snapshots and zeroing by the actual bitset size:

```c
set_bytes = (((unsigned)(nfds ? nfds : 1) + NFDBITS - 1) / NFDBITS) *
            sizeof(fd_mask);
```

- Replace `FD_ZERO()` on output buffers with `memset(..., set_bytes)` so the
  kernel never clears beyond the caller's allocation

**`src/syscall/syscall_proc.c`**

- Make `sys_brk(0)` a pure query again
- Model `rt_sigaction` using a 32-word userspace mask (`1024 / 32`)
- Zero all returned mask words and copy only the low word that MOS currently
  implements

**`include/ps/ps.h`, `src/ps/ps_fork.c`, `src/ps/ps_syscall.c`, `src/dev/tty.c`**

- Add per-process `root_path`
- Initialize it to `"/"` for new tasks
- Copy it across `fork()`
- Free it on task reap

**`src/fs/fs.c` and `src/syscall/syscall_fs.c`**

- Teach `resolve_path()` to prepend `root_path`
- Keep `cwd` jail-relative while VFS lookups use the rooted absolute path
- Add `sys_chroot()`
- Make `chdir()` and `fchdir()` update `cwd` within the jail
- Add overflow checks so rooted path prepends cannot scribble past `MAX_PATH`

### Result

- `sshd` no longer crashes on incoming connections
- OpenSSH privilege separation now completes its `chroot()` + UID/GID drop path
- SSH negotiation proceeds normally until modern-client compatibility checks
  reject old KEX / host-key / cipher algorithms

At this point the remaining interoperability issues are protocol-age policy
differences in the client, not kernel crashes in MOS.

### Key Lesson

When a legacy daemon crashes "later" inside libc, look for earlier kernel
overwrites of user-owned variable-length buffers.  Here the visible crash was
inside malloc, but the first real bug was a kernel `select()` implementation
that assumed every userspace `fd_set` was a full `FD_SETSIZE` object.  Once the
memory corruption was gone, the next blockers were ordinary ABI completeness
items (`chroot`, path-root semantics, legacy `rt_sigaction` layout).


---

## 2026-04-06 — GNU `screen` failed with `"No more PTYs"` and later hung blank

### Symptoms

- `screen` initially failed immediately with:
  ```text
  No more PTYs.
  ```
- After the first PTY fixes, `screen` got further but stopped with:
  ```text
  TIOCPKT ioctl: Function not implemented
  ```
- After that, `screen` started and cleared the display, but the UI stayed
  blank and keyboard input appeared dead.

### Investigation

#### Step 1 — `"No more PTYs"` did not mean PTY allocation failed

The kernel log showed:

- `open("/dev/ptmx")` succeeded
- `ioctl(TIOCGPTN)` succeeded
- `stat64("/dev/pts/0")` succeeded

Old `screen` reports `"No more PTYs"` whenever its `OpenPTY()` helper fails:

```c
if ((m = ptsname(f)) == NULL || grantpt(f) || unlockpt(f)) {
    close(f);
    return -1;
}
```

So the message was just a generic userspace error path, not evidence that the
kernel had run out of PTY slots.

#### Step 2 — `grantpt()` was falling back because `/dev/pts` did not look like `devpts`

Guest `glibc-2.3.2` `grantpt()` does:

1. `ptsname()`
2. `statfs("/dev/pts/N")`
3. If `f_type` is `DEVPTS_SUPER_MAGIC` (`0x1cd1`) or `DEVFS_SUPER_MAGIC`,
   return success
4. Otherwise fall back to old legacy PTY ownership code

Our `/dev/pts` implementation had no `statfs` callback, so glibc could not
recognize it as a real `devpts` mount. That kept `screen` on the wrong
compatibility path.

#### Step 3 — slave inode metadata also had to be persistent

`/dev/pts/N` metadata was synthesized from the current task doing `stat()`
instead of stored per PTY pair. That broke the Linux/Unix98 expectation that
the slave node has stable owner/mode state after `grantpt()`.

The PTY pair needed persistent:

- slave mode
- slave uid
- slave gid
- PTY lock state

#### Step 4 — `screen` requires packet mode on the PTY master

Once `grantpt()` and `unlockpt()` worked, `screen` failed on:

```text
ioctl(fd, TIOCPKT, ...) = -ENOSYS
```

`screen` enables `TIOCPKT` on the master and aborts if it is unsupported.
So basic Unix98 PTY allocation was not enough; packet mode support was also
required.

#### Step 5 — the first `TIOCPKT` implementation was too eager

After adding `TIOCPKT`, the trace showed:

```text
write(3, "\x1b[H\x1b[J...", 12)
read(6, "\x03", 4096) = 1
pselect(...)
```

That `0x03` was generated by our own PTY code as a synthetic
`TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE` control packet after `TCFLSH`.
`screen` then sat in its event loop waiting for real PTY traffic.

Removing those fabricated flush packets let startup progress further.

#### Step 6 — the final hang was a PTY buffer lifetime bug

The remaining blank-screen hang turned out not to be packet mode at all.
The real bug was PTY buffer ownership.

The original PTY pair lifecycle used `cyb_create()` and then tried to infer
when to drop hidden reader/writer references later in `pts_pair_check_free()`.
That was incorrect:

- `pts_slave_release()` closed `p->s2m` and `p->m2s` only when
  `slave_count` reached zero
- `pts_master_release()` also closed its side
- `pts_pair_check_free()` then closed additional implicit refs again

This created two problems:

1. **Double-close / premature free**
   The shared `cy_buf` objects could be freed while pointers remained stored
   in `p->s2m` / `p->m2s`.

2. **Use-after-free in poll/read paths**
   Later code such as:
   ```c
   cyb_isempty(p->s2m)
   cyb_set_poll_read(p->s2m, task)
   ```
   could touch a freed `cy_buf`, which explained the observed hang inside the
   lock path.

### Root Causes

1. **`/dev/pts` did not implement `statfs()` with `DEVPTS_SUPER_MAGIC`**, so
   old glibc `grantpt()` took the wrong path.

2. **Unix98 slave metadata was not stored per PTY pair**, so ownership/mode
   checks did not behave like Linux `devpts`.

3. **`TIOCPKT` support was missing**, but `screen` requires it.

4. **The first packet-mode implementation injected fake flush packets**, which
   confused `screen` during startup.

5. **PTY buffer ownership was wrong**, causing freed `cy_buf` pointers to be
   reused from `p->s2m` / `p->m2s`.

### Fixes

**`src/dev/ptmx.c`**

- Added `statfs()` for `/dev/pts` returning `0x1cd1`
- Fixed `/dev/pts` directory entries to use the real PTY index
- Stored persistent slave metadata in each PTY pair
- Made slave open take explicit `cy_buf` endpoint refs
- Switched Unix98 PTY buffer allocation to `cyb_create_named()`

**`src/dev/pty.c`**

- Mirrored the same buffer lifetime fixes for BSD PTYs

**`src/dev/pts.c`**

- Added persistent slave `chmod` / `chown` support
- Implemented `TIOCSPTLCK`, `TIOCGPTLCK`, `TCFLSH`, and `TIOCPKT`
- Removed synthetic packet-mode flush notifications
- Changed `pts_pair_check_free()` to destroy only the pair-owned named buffers,
  instead of closing imaginary hidden slave refs

**`include/fs/ioctl.h`**

- Added missing `TIOCPKT` constants

### Key Lesson

PTY bugs are often two layers deep: the first visible error may be a libc
compatibility check (`grantpt`, `statfs`, ownership metadata), but once that
is fixed, interactive programs like `screen` also depend on Linux-specific
TTY behavior such as `TIOCPKT`. Most importantly, shared PTY transport objects
must have explicit ownership. If buffer lifetime is inferred indirectly from
`master_open` / `slave_count`, it is very easy to free the transport while a
poll or read path still holds the raw pointer.


---

## 2026-04-05 — `man` doesn't work (stderr opened O_WRONLY)

### Symptom

Running `man` from the shell produced no output or hung.  Other programs that
use a pager (e.g. `less`, `more`) behaved the same way.

### Root Cause

In two places the kernel opens the initial stdio file descriptors for the
first userspace process and for bash spawned from a TTY:

```c
// exec.c — kinit_userspace()
fs_open("/dev/tty1", O_RDONLY, 0);  // fd 0 — stdin
fs_open("/dev/tty1", O_WRONLY, 0);  // fd 1 — stdout
fs_open("/dev/tty1", O_WRONLY, 0);  // fd 2 — stderr  ← bug
```

`stderr` (fd 2) was opened `O_WRONLY`.  Pagers like `less` (invoked by `man`)
use fd 2 as their terminal I/O channel when stdin/stdout are redirected to a
pipe.  They call `read(2, …)` to receive keystrokes from the user.  Because
fd 2 was write-only, those reads returned `-EBADF`, and the pager could not
receive any input — appearing to hang.

The same mistake existed in `tty.c` (`tty_bash_spawner`) for consoles spawned
after VT switch.

### Fix

Open fd 2 as `O_RDWR` so the TTY file descriptor is usable for both reading
and writing, matching real Linux behaviour:

```c
fs_open("/dev/tty1", O_RDWR, 0);  // fd 2 — stderr
```

Changed in `src/elf/exec.c` and `src/dev/tty.c`.

### Key Lesson

The TTY fd used as the controlling terminal must be `O_RDWR`.  Even though
`stderr` is conventionally write-only for application output, the kernel-side
fd must allow reads because pagers and interactive programs re-use fd 2 as
their terminal input channel when stdin is a pipe.

---

## 2026-04-05 — xinetd crash + "not enough memory" (ugetrlimit / setrlimit / fs_close)

### Symptoms

- `/usr/sbin/xinetd` segfaults immediately after startup:
  ```
  [899]: segfault: error code 4, address 0, eip 806114f, cmd /usr/sbin/xinetd
  ```
  The crash is a NULL dereference inside xinetd's own code, not in a library.

- Some other services log `"not enough memory"` at startup.

- Both symptoms appeared together and seemed contradictory: the value that
  fixed the crash caused the OOM, and vice versa.

### Investigation

#### Step 1 — identify which resources are queried

The kernel log showed `ugetrlimit(3)`, `ugetrlimit(4)`, and `ugetrlimit(7)`
being called. Resources 3 and 4 were returning `RLIM_INFINITY` (the default
fallback). Resource 7 (RLIMIT_NOFILE) was the only one with special handling.

#### Step 2 — narrow down by varying the return value

| `rl[0]` (cur)   | `rl[1]` (max)   | xinetd result | other services      |
| --------------- | --------------- | ------------- | ------------------- |
| `MAX_FD`(341)   | `RLIM_INFINITY` | segfault      | ok                  |
| `RLIM_INFINITY` | `MAX_FD`(341)   | **no crash**  | "not enough memory" |
| `RLIM_INFINITY` | `RLIM_INFINITY` | **no crash**  | "not enough memory" |

The value of `rl[0]` (soft limit / `rlim_cur`) was the decisive variable.

#### Step 3 — understand xinetd's startup sequence

Reading xinetd's source (`init.c`, `set_fd_limit()`):

```c
getrlimit(RLIMIT_NOFILE, &rl);
maxfd = rl.rlim_max;
if (rl.rlim_max == RLIM_INFINITY)
    rl.rlim_max = FD_SETSIZE;   // cap to 1024
if (rl.rlim_max > FD_SETSIZE)
    rl.rlim_max = FD_SETSIZE;
rl.rlim_cur = rl.rlim_max;
setrlimit(RLIMIT_NOFILE, &rl);  // sets both to min(rl_max, 1024)
ps.ros.max_descriptors = rl.rlim_max;
```

Then:
```c
for (fd = 3; fd < ps.ros.max_descriptors; fd++)
    Sclose(fd);   // close all fds above stderr
```

#### Step 4 — find why rl_cur matters

glibc reads `RLIMIT_NOFILE` **once at startup** (before `main()`) and uses
`rl_cur` to size its internal fd table. With `rl_cur = 341` glibc allocates
a 341-slot table. Then xinetd calls `setrlimit({1024, 1024})` which our
kernel was **silently ignoring** (setrlimit was a no-op). xinetd believed
`max_descriptors = 1024` and its close loop ran up to fd 1023. When fd 342
was accessed, glibc's table was out of bounds → NULL dereference → segfault.

With `rl_cur = RLIM_INFINITY`, glibc takes a different code path and does
not allocate a fixed-size table, so no out-of-bounds access.

#### Step 5 — understand the "not enough memory" service

That service does roughly:
```c
getrlimit(RLIMIT_NOFILE, &rl);
table = malloc(rl.rlim_cur * sizeof(ptr));
if (!table) log("not enough memory");
```
With `rl_cur = RLIM_INFINITY = 0xFFFFFFFF`, `malloc` fails. On real Linux,
`RLIMIT_NOFILE` defaults to `{1024, 1024}` — not `RLIM_INFINITY` — so this
never happens there.

#### Step 6 — the real Linux default is {1024, 1024}

Linux kernel default for `RLIMIT_NOFILE` is soft=1024, hard=1024.
With `{1024, 1024}`:
- glibc allocates a 1024-slot table
- xinetd gets `rl_max = 1024`, no capping needed, `max_descriptors = 1024`
- xinetd's `setrlimit({1024, 1024})` matches what we already return
- The table size and `max_descriptors` agree → no out-of-bounds
- `malloc(1024 * sizeof(...))` succeeds → no OOM

#### Step 7 — setrlimit must track values per-process

Even with the correct default, `setrlimit` must actually store the new
value. A child process forked after xinetd calls `setrlimit({1024, 1024})`
should inherit `{1024, 1024}`, not the default. Without tracking, children
read the initial default and glibc allocates the wrong table size again.

#### Step 8 — fs_close returning wrong errno

`fs_close` was returning `-ENOENT` for fds that are not open. Linux returns
`-EBADF` for all invalid `close()` calls. xinetd's close loop is:
```c
if (Sclose(fd) && errno != EBADF) { exit(1); }
```
`ENOENT ≠ EBADF` would cause a premature `exit(1)`. This is correct POSIX
behavior that was simply wrong in our implementation.

### Root Causes

1. **`RLIMIT_NOFILE` default was not `{1024, 1024}`** — we returned either
   `MAX_FD` (too small, glibc table mismatch) or `RLIM_INFINITY` (services
   OOM on malloc). The Linux default of `{1024, 1024}` satisfies both.

2. **`setrlimit` was a no-op** — it did not store the new limit. Children
   would inherit the initial default instead of what the parent set.

3. **`fs_close` returned `-ENOENT` for unopened fds** instead of `-EBADF`.

### Fixes

**`include/ps/ps.h`** — add per-process rlimit storage:
```c
#define RLIM_NLIMITS 16
#define RLIM_INFINITY 0xFFFFFFFFu

typedef struct { unsigned long rlim_cur, rlim_max; } rlimit_t;

struct _task_struct {
    ...
    rlimit_t rlimits[RLIM_NLIMITS];
};
```

**`src/ps/ps_fork.c`** (`_ps_create`) — initialize defaults:
```c
for (int i = 0; i < RLIM_NLIMITS; i++)
    task->rlimits[i] = {RLIM_INFINITY, RLIM_INFINITY};
task->rlimits[3] = {USER_STACK_PAGES * PAGE_SIZE, ...}; // RLIMIT_STACK
task->rlimits[4] = {0, 0};                              // RLIMIT_CORE
task->rlimits[7] = {1024, 1024};                        // RLIMIT_NOFILE
```
Fork inherits automatically because `fork_alloc_child` does `*task = *cur`.

**`src/syscall/syscall_proc.c`** — wire up reads and writes:
```c
int sys_ugetrlimit(int resource, void *limit) {
    task_struct *cur = CURRENT_TASK();
    unsigned long *rl = limit;
    if (rl && resource >= 0 && resource < RLIM_NLIMITS) {
        rl[0] = cur->rlimits[resource].rlim_cur;
        rl[1] = cur->rlimits[resource].rlim_max;
    }
    return 0;
}

int sys_setrlimit(int resource, void *limit) {
    task_struct *cur = CURRENT_TASK();
    unsigned long *rl = limit;
    if (rl && resource >= 0 && resource < RLIM_NLIMITS) {
        cur->rlimits[resource].rlim_cur = rl[0];
        cur->rlimits[resource].rlim_max = rl[1];
    }
    return 0;
}
```

**`src/fs/fs.c`** (`fs_close`) — return correct errno:
```c
if (cur->fds[fd].used == 0)
    return -EBADF;   // was -ENOENT
```

### Key Lesson

When a service crashes at address 0 right after `execve`, check what value
glibc uses from `ugetrlimit` at startup — glibc sizes internal tables from
`rl_cur` before `main()` runs, and that sizing is permanent. Any subsequent
`setrlimit` call does not resize the already-allocated table. The kernel's
default must match the real Linux default (`{1024, 1024}` for RLIMIT_NOFILE)
so all userspace assumptions are met from the start.
