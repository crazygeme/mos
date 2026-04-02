# TTY / PTY Subsystem

**Source:** `src/dev/tty.c`, `src/dev/tty_ldisc.c`, `src/dev/pts.c`  
**Headers:** `include/hw/tty.h`, `src/dev/tty_ldisc.h` (internal)

---

## Overview

The TTY subsystem provides:

- **10 virtual consoles** (`/dev/tty0`..`/dev/tty9`) backed by the framebuffer.
- **PTY pairs** (`/dev/ptyp0`..`/dev/ptypf` master, `/dev/ttyp0`..`/dev/ttypf` slave), BSD-style as used on RH9 / Linux 2.4.
- A shared **line discipline** (`tty_ldisc.c`) used by both virtual consoles and PTY slaves.

---

## 1. Virtual consoles (`tty.c`)

### `tty_state` — per-TTY state

```c
typedef struct {
    unsigned max_row, max_col;   // framebuffer dimensions in characters
    spinlock_t lock;             // output lock; also protects cursor
    int cursor;                  // linear position: row*max_col + col
    struct termios termios;      // terminal settings
    unsigned pgrp;               // foreground process group
    int ansi_flag;               // 1 while inside an ANSI escape sequence
    char ansi_buf[10];           // ANSI parameter accumulator
    int ansi_idx;
    tty_canon_t canon;           // canonical line buffer (256 bytes)
    cy_buf *kb_buf;              // keyboard input ring buffer
    tty_cell_t *cells;           // main screen cell buffer (rows*cols)
    tty_cell_t *alt_cells;       // alternate screen buffer (?1049h/l)
    int tty_idx;                 // 0..TTY_MAX_VDEV-1
    unsigned bash_pid;           // PID of bash on this TTY (0 = none)
    int open_count;              // number of open file structs
    int released;                // set when last fd is closed
    int saved_cursor;            // ESC[s / ESC[u saved position
    int scroll_top, scroll_bot;  // DECSTBM scrolling region
    int cursor_hidden;           // ?25l: hide hardware cursor
    int no_wrap;                 // ?7l: disable auto-wrap
    unsigned fg_color, bg_color; // current SGR colors (ARGB)
    int kb_mode;                 // K_XLATE / K_RAW
} tty_state;
```

`TTY_MAX_VDEV = 10` instances are statically allocated.  
`active_tty_idx` tracks which TTY owns the real framebuffer.  
`tty_switch_lock` serialises TTY switches and keyboard routing.

### Device nodes

Registered at boot by `tty_dev_register` (via `DEV_INIT`):

| Device                   | Major | Minor | Maps to                          |
| ------------------------ | ----- | ----- | -------------------------------- |
| `/dev/tty0`..`/dev/tty9` | 4     | 0–9   | `ttys[minor]`                    |
| `/dev/tty`               | 5     | 0     | `ttys[0]` (controlling terminal) |
| `/dev/console`           | 5     | 1     | `ttys[0]` (system console)       |

`tty_fs_init` (via `KERNEL_INIT 0`) allocates per-TTY resources (cell buffers, keyboard ring buffers, alternate screen buffers) before any output occurs.

### Normal character output — full pipeline

```
write(fd, buf, n)
  └─ tty_fs_write               [checks released, TOSTOP/SIGTTOU]
       └─ tty_do_write          [holds state->lock for entire buffer]
            └─ for each byte: output_char
                 │
                 │  c_oflag processing (OPOST must be set):
                 │    OLCUC : tolower(c)→toupper(c)
                 │    ONLCR : '\n' → write "\r\n" instead
                 │    OCRNL : '\r' → write "\n" instead
                 │    ONOCR : '\r' at col 0 → discard
                 │
                 └─ tty_raw_write(state, &c, 1)
                      └─ process_one_char(state, c)  → new_cursor_pos
                           │
                           │  if ansi_flag: ansi_feed(state, c); return cursor
                           │
                           │  control characters:
                           │    '\n'  if at scroll_bot → tty_roll_region; stay row
                           │          otherwise         → cursor down one row, same col
                           │    '\r'  → cursor to col 0, same row
                           │    '\b'  → cursor -= 1 (clamp 0)
                           │    '\t'  → fill spaces to next 8-column tab stop,
                           │           advance cursor past them
                           │    0x0c  → tty_do_clear; cursor unchanged
                           │    0x1b  → ansi_begin; cursor unchanged
                           │
                           │  printable char:
                           │    vga_putchar(state, CUR_ROW, CUR_COL, c)
                           │      → state->cells[idx] = {c, fg_color, bg_color}
                           │      → if active TTY: fb_putcell(&cell, col, row)
                           │    if no_wrap && at last col → cursor stays
                           │    otherwise                 → cursor += 1
                           │
                           └─ cursor_set(state, new_pos)
                                → state->cursor = new_pos
                                → while cursor >= MAX_CHARS: tty_roll_line()
                                                              cursor -= MAX_COL
            │
            └─ tty_hw_cursor(state, state->cursor)   [once, after whole buffer]
```

`tty_raw_write` intentionally defers the hardware cursor update to after the whole buffer is written — `process_one_char` + `cursor_set` only update `state->cursor` (the logical position); `tty_hw_cursor` is called once at the end by `tty_do_write`.

### Cursor management

The cursor is a single integer `state->cursor` = `row * max_col + col`. Two separate cursors exist:

| Name                | Type                | Meaning                                                |
| ------------------- | ------------------- | ------------------------------------------------------ |
| `state->cursor`     | `int` (per-TTY)     | Logical cursor, valid for every TTY even inactive ones |
| `_displayed_cursor` | `unsigned` (global) | Position of the cursor mark actually drawn on screen   |

**Moving the cursor** (`cursor_set` / `cursor_forward`):

```c
cursor_set(state, pos):
    state->cursor = pos
    while state->cursor >= MAX_CHARS:   // past last cell
        tty_roll_line(state)            // scroll whole screen up one line
        state->cursor -= MAX_COL        // back up one row

cursor_forward(state, pos):
    cursor_set(state, pos)
    tty_hw_cursor(state, state->cursor) // also sync hardware
```

`cursor_set` is used inside `tty_raw_write` (hardware not synced yet). `cursor_forward` is used by `tty_default_emit_unsafe` (printk path) where each character must move the hardware cursor immediately.

**Syncing hardware** (`tty_hw_cursor`):

```c
tty_hw_cursor(state, pos):
    if cursor_hidden || tty_idx != active_tty_idx: return
    fb_cursor_update(_displayed_cursor, pos, state->cells, MAX_COL)
    _displayed_cursor = pos
```

`fb_cursor_update` redraws the old cursor cell to erase the highlight, then draws the new cell with the cursor color overlay. `_displayed_cursor` tracks the screen position so the old highlight can be erased; it is only valid for the active TTY.

**Scroll and cursor**: `tty_roll_line` / `tty_roll_region` must erase the on-screen cursor before shifting pixels (otherwise a stale cursor artifact remains). They call `fb_cursor_erase(_displayed_cursor, ...)` before `fb_scroll_*_px()`.

**Saved cursor** (`ESC[s` / `ESC[u`): stored in `state->saved_cursor`. Does not save/restore colors or attributes — cursor position only.

**Hidden cursor** (`ESC[?25l` / `ESC[?25h`): sets `cursor_hidden`. When hidden, `tty_hw_cursor` is a no-op; `_displayed_cursor` stops tracking. On un-hide (`?25h`), `tty_hw_cursor` is called explicitly to redraw the cursor at the current position.

### Input path

```
read(fd, buf, n)
  └─ tty_fs_read
       ├─ ICANON set → canon_readline → tty_ldisc_canon_readline
       │    └─ processes: ERASE (^H), KILL (^U), INTR (^C), QUIT (^\), SUSP (^Z)
       │    └─ line complete on \n / VEOL / VEOL2 → tty_canon_drain to user
       └─ ICANON clear → raw_read
            └─ blocks until VMIN bytes available in kb_buf
```

Keyboard bytes are injected via `tty_active_kb_put(c)` (called from the keyboard IRQ handler). Signal characters (`VINTR`, `VQUIT`, `VSUSP`) are intercepted early and sent to the foreground process group via `ps_send_signal_pgrp`.

### ANSI escape sequences

The state machine is a two-phase parser:
1. `\x1b` sets `ansi_flag = 1`, clears `ansi_buf`.
2. Subsequent bytes accumulate in `ansi_buf` until a letter terminates the sequence.

Supported sequences (via `ansi_feed`):

| Sequence                 | Effect                                        |
| ------------------------ | --------------------------------------------- |
| `ESC[H` / `ESC[row;colH` | Cursor position (CUP)                         |
| `ESC[A/B/C/D`            | Cursor up/down/forward/back                   |
| `ESC[J`                  | Erase display (0/1/2/3)                       |
| `ESC[K`                  | Erase line (0/1/2)                            |
| `ESC[L` / `ESC[M`        | Insert / delete lines (IL/DL)                 |
| `ESC[P` / `ESC[@`        | Delete / insert characters (DCH/ICH)          |
| `ESC[m`                  | SGR: colors 30–37 (fg), 40–47 (bg), 0 = reset |
| `ESC[r`                  | DECSTBM: set scroll region                    |
| `ESC[s` / `ESC[u`        | Save / restore cursor                         |
| `ESC[?7h/l`              | Auto-wrap on/off                              |
| `ESC[?25h/l`             | Show/hide cursor                              |
| `ESC[?1049h/l`           | Enter/exit alternate screen buffer            |
| `ESC M`                  | RI: reverse index (scroll up at top)          |
| `ESC c`                  | RIS: full terminal reset                      |

### TTY switching

Triggered by Ctrl+Alt+F1..F10 from the keyboard DSR. All operations are non-blocking (no sleep, no malloc) since this runs from IRQ context.

```
tty_switch(n):
    if n == active_tty_idx: return           // no-op

    spinlock_lock(&tty_switch_lock)

        active_tty_idx = n
        this_ttys      = &ttys[n]            // atomic flip — subsequent writes
                                             // now target new TTY's cells[]
        _displayed_cursor = ttys[n].cursor   // sync global cursor tracker

        fb_redraw(ttys[n].cells,             // repaint entire framebuffer
                  ttys[n].max_col,           // from new TTY's cell buffer
                  ttys[n].max_row,
                  ttys[n].cursor)

    spinlock_unlock(&tty_switch_lock)

    if n > 0:                                // TTY 0 is always owned by init
        need_spawn = (bash_pid == 0)
                  || ps_find_process(bash_pid) == NULL
                  || ps_find_process(bash_pid)->status == ps_dying
        if need_spawn:
            ttys[n].bash_pid = ps_create(tty_bash_spawner, &ttys[n],
                                         ps_normal, ps_kernel)
```

**Why no explicit save on switch-out**: every TTY always keeps its `cells[]` in sync regardless of whether it is active. Inactive-TTY writes update `cells[]` but skip `fb_putcell` (the `tty_idx == active_tty_idx` guard in `vga_putchar`). So the cell buffer is always an accurate shadow of what the TTY would show. `fb_redraw` on switch-in just repaints it all.

**`tty_switch_lock` scope**: held only for the pointer flip + `fb_redraw`. The bash spawn is outside the lock because `ps_create` may allocate memory. `tty_active_kb_put` also acquires this lock briefly to snapshot `this_ttys` — this guarantees keyboard bytes are never routed to the old TTY after the flip.

**Bash spawner** (`tty_bash_spawner`):

```
ps_create(tty_bash_spawner, state, ps_normal, ps_kernel)
  └─ new kernel task:
       cur->root   = state->parent->root   // inherit root filesystem
       sb_get(root)
       cur->parent = state->parent         // so waitpid works
       cur->cwd    = "/root"
       ps_update_tss(cur + PAGE_SIZE)      // set esp0 for user-mode entry

       fd 0 = open("/dev/ttyn", O_RDONLY)
       fd 1 = open("/dev/ttyn", O_WRONLY)
       fd 2 = open("/dev/ttyn", O_WRONLY)

       if fs_stat("/bin/bash") == 0:
           sys_execve("/bin/bash",
                      argv = {"/bin/bash", "-l", NULL},
                      envp = {"PATH=/bin:/usr/bin:/sbin",
                              "TERM=linux", "HOME=/root",
                              "LANG=en_US", NULL})
       // bash not found → task exits; bash_pid stays set
       // next switch-in detects dead PID and re-spawns
```

### Supported ioctls

| ioctl                       | Effect                                |
| --------------------------- | ------------------------------------- |
| `FIONREAD`                  | Return byte count in keyboard buffer  |
| `TCGETS`                    | Get termios                           |
| `TCSETS` / `TCSETSW`        | Set termios                           |
| `TCSETSF`                   | Flush input, set termios              |
| `TIOCGWINSZ`                | Get window size (rows × cols)         |
| `TIOCSWINSZ`                | Accept silently (hardware-fixed size) |
| `TIOCGPGRP` / `TIOCSPGRP`   | Get/set foreground process group      |
| `TIOCSCTTY`                 | Set controlling terminal              |
| `KDGKBTYPE`                 | Return KB_101                         |
| `KDGKBMODE` / `KDSKBMODE`   | Get/set keyboard translation mode     |
| `KDGKBENT` / `KDSKBENT`     | Get/set keymap entry                  |
| `KDGKBSENT` / `KDSKBSENT`   | Get/set string entry                  |
| `KDGKBDIACR` / `KDSKBDIACR` | Dead-key table (stub)                 |
| `TIOCLINUX` / `KDSIGACCEPT` | Accept silently                       |

---

## 2. Line discipline (`tty_ldisc.c`)

Shared between virtual consoles (`tty.c`) and PTY slaves (`pts.c`).

### `tty_canon_t`

```c
#define TTY_CANON_BUF_SIZE 256

typedef struct {
    char buf[TTY_CANON_BUF_SIZE];
    int  len;
} tty_canon_t;
```

### Default termios

```c
.c_iflag = ICRNL
.c_oflag = OPOST | ONLCR
.c_cflag = B38400 | CS8
.c_lflag = IXON | ISIG | ICANON | ECHO | ECHOE | ECHOCTL | ECHOKE
```

### `tty_ldisc_canon_readline`

```c
int tty_ldisc_canon_readline(tty_canon_t *canon, const struct termios *tc,
                             cy_buf *buf, int check_eof, unsigned pgrp,
                             tty_ldisc_echo_fn echo, void *ctx);
```

Reads one canonical line from `buf` into `canon`. Returns:
- `1` — line complete (newline / VEOL / VEOL2 seen)
- `0` — EOF (master closed for PTY; VEOF for keyboard)
- `-1` — EINTR (signal character received)

Character processing per iteration:
1. `tty_input_translate`: apply `ISTRIP`, `ICRNL`, `INLCR`, `IGNCR`, `IUCLC`.
2. `isig_char`: check `VINTR`/`VQUIT`/`VSUSP` → `ps_send_signal_pgrp`.
3. `VERASE`: erase last character, echo `\b \b` (if `ECHOE`).
4. `VKILL`: erase whole line, echo backspaces (if `ECHOK`) or VKILL char + newline.
5. Otherwise: append to canon, echo if `ECHO`; complete line if `\n`/`VEOL`.

### `tty_input_translate`

Applies c_iflag transformations to one byte:
- `ISTRIP`: strip bit 7.
- `\r` + `IGNCR` → discard (`-1`).
- `\r` + `ICRNL` → `\n`.
- `\n` + `INLCR` → `\r`.
- `IUCLC` → convert to lowercase.

---

## 3. PTY pairs (`pts.c`)

BSD-style pseudo-terminal pairs, compatible with RH9 / Linux 2.4 naming.

### Device nodes

| Device                     | Major | Minor | Role       |
| -------------------------- | ----- | ----- | ---------- |
| `/dev/ptyp0`..`/dev/ptypf` | 2     | 0–15  | PTY master |
| `/dev/ttyp0`..`/dev/ttypf` | 3     | 0–15  | PTY slave  |

### `pts_pair` — per-pair state

```c
typedef struct {
    int idx;
    int used;
    spinlock_t lock;
    struct termios termios;
    struct winsize winsize;
    unsigned pgrp;
    cy_buf *m2s;        // master→slave pipe
    cy_buf *s2m;        // slave→master pipe
    tty_canon_t canon;  // canonical input buffer for slave reads
    int master_open;
    int slave_count;
} pts_pair;
```

`MAX_PTS = 16` pairs are statically allocated.

### Data flow

```
master write  →  m2s ring buf  →  slave read  (line-disciplined)
slave write   →  s2m ring buf  →  master read (raw)
```

Slave reads go through `tty_ldisc_canon_readline` / raw_read (same logic as virtual consoles). When the master closes, an EOF sentinel (`0xFF`) is injected into m2s so the slave read returns 0.

### Unix98 ioctls (on master)

| ioctl                       | Effect                                   |
| --------------------------- | ---------------------------------------- |
| `TIOCGPTN`                  | Return PTY index                         |
| `TIOCSPTLCK` / `TIOCGPTLCK` | Lock/query slave (stub, always unlocked) |

Standard `TCGETS`/`TCSETS`/`TIOCGWINSZ`/`TIOCSWINSZ`/`TIOCGPGRP`/`TIOCSPGRP` are supported on both master and slave.
