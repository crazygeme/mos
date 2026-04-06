# Bug Fix Journal

A running log of non-obvious bugs, their symptoms, root causes, and fixes.
Each entry explains the reasoning so the same mistake is not repeated.

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
