# Booting RedHat 9 Userspace on MOS — Debug Journal

This document records the bugs found and fixed while bringing up a real RH9
(`/sbin/init`) userspace on the MOS kernel.  The end goal was a working login
prompt on tty1.  Six independent bugs had to be fixed in sequence.

---

## 1. Wall clock time was always near epoch 0

### Symptom
At every boot, `/etc/rc.sysinit` ran `makewhatis` to rebuild the `whatis`
database.  On a real Linux kernel the rebuild is skipped because the database
file is already newer than the man pages.

### Root cause
`time_init()` read the real-time clock into `boot_epoch` but never used it.
`time_wall_us()` returned `time_now_us() + g_wall_offset_us`, where
`g_wall_offset_us` was 0.  So the kernel reported the current time as
"seconds since boot" (~30 s), not the actual Unix timestamp.

Files written by MOS therefore got an mtime of ~30 seconds.  The RH9 man
pages on disk have 2003-era mtimes (~1.07 billion seconds).  `makewhatis`
compared the two and always concluded the database was stale.

### Fix — `src/hw/time.c`
Initialize `g_wall_offset_us` from the RTC value before starting the PIT
interrupt:

```c
boot_epoch = rtc_get_time();
g_wall_offset_us = (long long)boot_epoch * 1000000LL;
int_register(0x20, time_process, 0, 0);
```

### Side note
After the clock fix the `whatis` database file that had been copied from a
real Linux VM (mtime March 2025) was still considered stale, because MOS now
correctly reported a 2026 date.  The workaround was to `touch` the database
file inside `rh9.qcow2` after mounting it via `qemu-nbd`.

---

## 2. `rt_sigsuspend` spun in an infinite loop

### Symptom
`/sbin/init` (PID 1) called `rt_sigsuspend` in a tight loop, burning CPU and
never making progress.

### Root cause
The sequence:

1. A child process exits → `SIGCHLD` is added to `init`'s `sig_pending`.
2. `rt_sigsuspend` sets a temporary mask that *unmasks* SIGCHLD, then calls
   `ps_signal_wait()`.  `ps_signal_wait` returns immediately because SIGCHLD
   is pending and unmasked.
3. **Bug**: before returning `-EINTR`, `sys_rt_sigsuspend` restored the old
   mask (which *blocks* SIGCHLD).
4. `do_signal` ran next, saw SIGCHLD masked → `deliverable = 0` → returned
   without clearing `sig_pending`.
5. Next call to `rt_sigsuspend` repeated step 1 onwards — infinite loop.

### Fix — `src/syscall/syscall_proc.c` and `include/ps/ps.h`

Added two fields to `signal_context`:

```c
sigset_t saved_sigmask;   /* mask saved by sigsuspend */
int      restore_sigmask; /* if set, restore saved_sigmask after delivery */
```

`sys_rt_sigsuspend` no longer restores the mask before returning.  Instead it
stores the saved mask for `do_signal` to restore after it clears the signal:

```c
ps_signal_wait();
cur->signal->saved_sigmask = saved_mask;
cur->signal->restore_sigmask = 1;
return -EINTR;
/* removed: cur->signal->sig_mask = saved_mask; */
```

`do_signal` was updated to restore `saved_sigmask` after clearing the pending
signal, for all three delivery paths: `SIG_IGN`, `SIG_DFL`-ignore, and
user-handler (via the signal frame so `sigreturn` restores the right mask).

---

## 3. `chown` on devfs nodes returned `EACCES`

### Symptom
`mingetty` exited with status 100 immediately after being spawned.

### Root cause
`mingetty` calls `chown(tty_path, uid, gid)` to claim ownership of its TTY
before opening it.  `fs_chown()` returned `-EACCES` whenever the inode had no
`.chown` operation — which is the case for all virtual devfs nodes (tty, pts,
null, etc.).  `mingetty` treated any non-zero return from chown as fatal.

### Fix — `src/fs/fs.c`
Return `0` instead of `-EACCES` when the inode has no chown/fchown
implementation:

```c
if (!fp->f_inode || !fp->f_inode->i_op || !fp->f_inode->i_op->chown) {
    fs_put_file(fp);
    return 0;   /* was -EACCES */
}
```

Applied to `fs_chown`, `fs_fchown`, `fs_chmod`, and `fs_fchmod`.

---

## 4. `chmod` on devfs nodes returned `EACCES`

### Symptom
After the chown fix, `mingetty` exited again — this time failing on `chmod`.

### Root cause
Same pattern as above: `fs_chmod` returned `-EACCES` when the inode has no
`.setattr` operation.

### Fix
Same fix as `fs_chown`: return `0` when `.setattr` is not implemented.

---

## 5. TTY read returned EOF immediately after reopen

### Symptom
After chown and chmod were fixed, `mingetty` still exited.  The kernel log
showed it opening the TTY, reading from it, and getting 0 bytes (EOF)
immediately.

### Root cause
When the last file descriptor referencing a TTY is closed, the TTY driver
writes an EOF sentinel byte into `kb_buf` so that any blocked reader unblocks.
When the TTY was subsequently reopened, `released` was cleared but the EOF
sentinel byte remained in `kb_buf`.  The next `read` consumed that byte and
returned 0.

### Fix — `src/dev/tty.c`, `tty_open_state()`
Flush `kb_buf` on reopen:

```c
state->released = 0;
cyb_flush(state->kb_buf);   /* added */
__sync_add_and_fetch(&state->open_count, 1);
```

---

## 6. TTY device index was off by one — login prompt not visible

### Symptom
The kernel log showed `mingetty` on `/dev/tty1` successfully writing
`"localhost login: "` and blocking on `read`, but nothing appeared on screen.

### Root cause
The internal TTY array `ttys[]` was 0-indexed.  `DEFAULT_TTY = 0` meant
`ttys[0]` was the active (visible) console.  But `/dev/tty1` (major 4, minor
1) was opening `ttys[1]`, which is an *inactive* back-buffer.  Output went
into that buffer and was never rendered.

On real Linux, `/dev/tty1` is the *first* virtual console — the active one at
boot.

### Fix — `src/dev/tty.c`

**Part A — 1-based minor mapping in `tty_cdev_open`** (from earlier in the
session):

```c
if (major == 4) {
    if (minor < 1 || minor - 1 >= TTY_MAX_VDEV)
        return NULL;
    state = &ttys[minor - 1];   /* tty1 → ttys[0], tty2 → ttys[1], … */
}
```

**Part B — full 1-based overhaul** (this session):

| Location | Before | After |
|---|---|---|
| `DEFAULT_TTY` | `0` | `1` |
| `tty_init()` | `tty_idx = i` | `tty_idx = i + 1` |
| `tty_switch()` bound check | `n < 0 \|\| n >= TTY_SWITCH_COUNT` | `n < 1 \|\| n > TTY_SWITCH_COUNT` |
| `tty_switch()` array access | `&ttys[n]` | `&ttys[n - 1]` |
| `tty_switch()` bash spawn guard | `if (n > 0)` | `if (n > 1)` |
| `tty_cdev_open()` major==5 | `&ttys[DEFAULT_TTY]` | `&ttys[DEFAULT_TTY - 1]` |
| `tty_fs_init()` | `&ttys[DEFAULT_TTY]` | `&ttys[DEFAULT_TTY - 1]` |
| `tty_dev_register()` | minors 0..9, `/tty0`..`/tty9` | minors 1..10, `/tty1`..`/tty10` |
| `kinit_userspace()` in `exec.c` | `fs_open("/dev/tty0", …)` | `fs_open("/dev/tty1", …)` |

**`src/hw/keyboard.c`** — Ctrl+*N* now calls `tty_switch(N)` instead of
`tty_switch(N-1)`:

```c
/* Ctrl+1..0: switch virtual terminal (scancodes 2..11 → tty 1..10) */
if (!release && ctrl && code >= 2 && code <= 11) {
    tty_switch((int)(code - 1));   /* was code - 2 */
    return;
}
```

`tty_bash_spawner` required no changes because it already used
`state->tty_idx` to construct `/dev/tty%d`.

---

## Result

After all six fixes, RH9's SysV init boot sequence completes successfully:

1. `rc.sysinit` runs (skipping `makewhatis` because the clock is correct).
2. `init` spawns `mingetty` on tty1.
3. `mingetty` claims the TTY via chown/chmod (now silently succeeding).
4. `mingetty` reopens the TTY and reads from it without hitting a stale EOF.
5. The login prompt appears on the active console.

**First milestone achieved: RedHat 9.0 userspace successfully boots on MOS.**
