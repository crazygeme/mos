# Framebuffer / VGA Driver

**Source:** `src/hw/vga.c`, `src/hw/vga_bochs.c`, `src/hw/vmsvga.c`  
**Header:** `include/hw/vga.h`

---

## Overview

The framebuffer layer provides a hardware-independent character-cell display for the TTY subsystem. It consists of:

1. A **dispatcher** (`vga.c`) that probes available drivers and forwards all calls through the active one.
2. A **Bochs/QEMU VBE driver** (`vga_bochs.c`) for the Bochs Graphics Adapter (BGA) / QEMU stdvga.
3. A **VMware SVGA2 driver** (`vmsvga.c`) for VMware and QEMU's `-device vmsvga`.

Resolution and depth are compile-time constants in `include/config.h`:

```c
#define VGA_RESOLUTION_X  720
#define VGA_RESOLUTION_Y  540
#define VGA_COLOR_DEPTH    32   /* bits per pixel */
```

---

## 1. Cell type

```c
typedef struct {
    char     ch;   /* character (ASCII) */
    unsigned fg;   /* foreground color (ARGB) */
    unsigned bg;   /* background color (ARGB) */
} tty_cell_t;
```

Colors are 32-bit ARGB values. Predefined constants:

```c
#define ARGB(a,r,g,b)  ((0xFF*0x1000000ULL) + (r<<16) + (g<<8) + b)

VGA_COLOR_BLACK    ARGB(ff, 00, 00, 00)
VGA_COLOR_WHITE    ARGB(ff, ff, ff, ff)
VGA_COLOR_RED      ARGB(ff, ff, 00, 00)
VGA_COLOR_GREEN    ARGB(ff, 00, ff, 00)
VGA_COLOR_BLUE     ARGB(ff, 00, 00, ff)
VGA_COLOR_YELLOW   ARGB(ff, ff, ff, 00)
VGA_COLOR_CYAN     ARGB(ff, 00, ff, ff)
VGA_COLOR_MAGENTA  ARGB(ff, ff, 00, ff)
VGA_COLOR_GRAY     ARGB(ff, aa, aa, aa)
```

---

## 2. Driver interface (`fb_drv_t`)

```c
typedef struct {
    int  (*probe)(void);
    void (*get_char_dims)(unsigned *cols, unsigned *rows);
    void (*putcell)(const tty_cell_t *cell, int col, int row);
    void (*redraw)(const tty_cell_t *cells, unsigned cols, unsigned rows,
                   unsigned cursor_pos);
    void (*cursor_update)(unsigned old_pos, unsigned new_pos,
                          const tty_cell_t *cells, unsigned cols);
    void (*scroll_line_px)(void);
    void (*scroll_region_px)(unsigned top_row, unsigned bot_row);
    void (*insert_lines_px)(unsigned row, unsigned bot_row, unsigned n);
    void (*delete_lines_px)(unsigned row, unsigned bot_row, unsigned n);
    void (*clear_screen)(void);
    void (*change_font)(const char *name);
    int  (*is_char_visible)(unsigned char c);
    void (*cursor_erase)(unsigned pos, const tty_cell_t *cells, unsigned cols);
} fb_drv_t;
```

| Operation          | Called when                                                  |
| ------------------ | ------------------------------------------------------------ |
| `probe`            | `fb_init()` — returns 1 if hardware detected and initialised |
| `get_char_dims`    | TTY init — queries usable columns/rows                       |
| `putcell`          | Single character changed                                     |
| `redraw`           | Full screen refresh (TTY switch, alt-screen exit)            |
| `cursor_update`    | Cursor moved — redraws old and new cell                      |
| `cursor_erase`     | Erase cursor highlight at a position                         |
| `scroll_line_px`   | Whole-screen scroll by one line                              |
| `scroll_region_px` | DECSTBM region scroll by one line                            |
| `insert_lines_px`  | IL escape: insert n blank lines at row                       |
| `delete_lines_px`  | DL escape: delete n lines at row                             |
| `clear_screen`     | Erase entire display                                         |
| `change_font`      | Load named bitmap font (e.g. `"vga16"`)                      |
| `is_char_visible`  | Check if a character has a glyph in the loaded font          |

---

## 3. Dispatcher (`vga.c`)

`fb_init()` probes drivers in priority order and binds the global `_drv`:

```c
void fb_init(void)
{
    if (vmsvga_drv.probe()) { _drv = &vmsvga_drv; return; }
    if (bochs_drv.probe())  { _drv = &bochs_drv;  return; }
}
```

VMware SVGA2 is tried first because QEMU emulates both BGA and VMSVGA simultaneously when `-device vmsvga` is used; VMSVGA offers hardware-accelerated rect-copy/fill.

All public `fb_*` functions are thin wrappers that forward through `_drv` if non-NULL:

```c
void fb_putcell(const tty_cell_t *cell, int col, int row)
{
    if (_drv) _drv->putcell(cell, col, row);
}
```

---

## 4. Bochs/QEMU VBE driver (`vga_bochs.c`)

**PCI ID:** vendor `0x1234`, device `0x1111`  
**Interface:** Bochs Graphics Adapter (BGA) I/O ports `0x01CE` (index) / `0x01CF` (data)

### Initialisation (`bochs_probe`)

1. Check BGA availability: write/read `VBE_DISPI_ID5 (0xB0C5)` to index register 0.
2. Set video mode: `VGA_RESOLUTION_X × VGA_RESOLUTION_Y × VGA_COLOR_DEPTH` with LFB enabled.
3. Scan PCI for device `0x1234:0x1111`, read BAR0 as framebuffer physical base.
4. Identity-map framebuffer pages via `mm_map_io`.
5. Load font `"vga16"`, compute `_window_char_width/height`.

### Rendering

All rendering is CPU-side pixel writes into the linear framebuffer (`_fb_buffer`).

- `render_cell`: rasterises a `tty_cell_t` using the bitmap font glyph, writing fg/bg pixels.
- `render_cursor_cell`: overlays a cursor glyph (from `font->cursor_glyphs`) on top of the character.

Scroll operations use `memmove` on the framebuffer bytes to shift pixel rows.

---

## 5. VMware SVGA2 driver (`vmsvga.c`)

**PCI ID:** vendor `0x15AD`, device `0x0405`  
**Registers:** I/O ports at BAR0 — index port at `_iobase+0`, value port at `_iobase+1`  
**FIFO:** MMIO ring buffer at BAR2  
**Framebuffer:** MMIO at BAR1

### Initialisation (`vmsvga_probe`)

1. Scan PCI for `0x15AD:0x0405`; extract BAR0 (I/O), BAR1 (FB), BAR2 (FIFO).
2. Write `SVGA_ID_2 (0x90000002)` to `SVGA_REG_ID`; verify it reads back.
3. Map FIFO pages, initialise FIFO header (`MIN`/`MAX`/`NEXT_CMD`/`STOP`).
4. Set `SVGA_REG_WIDTH`, `HEIGHT`, `BPP`, `ENABLE=1`, `CONFIG_DONE=1`.
5. Read `SVGA_REG_CAPABILITIES` into `_caps`.
6. Map framebuffer pages, zero it, load font `"vga16"`.

### FIFO commands

| Command                  | Usage                                                      |
| ------------------------ | ---------------------------------------------------------- |
| `SVGA_CMD_UPDATE` (1)    | Mark a pixel rectangle dirty → host repaints               |
| `SVGA_CMD_RECT_COPY` (3) | Hardware blit: scroll lines                                |
| `SVGA_CMD_RECT_FILL` (2) | Hardware fill: clear regions (if `SVGA_CAP_RECT_FILL` set) |

`RECT_COPY`/`RECT_FILL` are followed by `fifo_sync()` (spin on `SVGA_REG_BUSY`) to ensure ordering before subsequent writes. `SVGA_CMD_UPDATE` is fire-and-forget.

### Rendering vs. Bochs

Pixel rendering into VRAM is identical to the Bochs driver (same `render_cell`/`render_cursor_cell` logic). The key difference is scroll/insert/delete operations:

| Operation     | Bochs                     | VMSVGA                                           |
| ------------- | ------------------------- | ------------------------------------------------ |
| Scroll line   | CPU `memmove` on FB bytes | `SVGA_CMD_RECT_COPY`                             |
| Scroll region | CPU `memmove`             | `SVGA_CMD_RECT_COPY`                             |
| Insert lines  | CPU `memmove`             | `SVGA_CMD_RECT_COPY`                             |
| Delete lines  | CPU `memmove`             | `SVGA_CMD_RECT_COPY`                             |
| Clear         | CPU `memset`              | `SVGA_CMD_RECT_FILL` (or CPU fill if cap absent) |
| Putcell       | Write pixels              | Write pixels + `SVGA_CMD_UPDATE`                 |

---

## 6. Lifecycle

```
tty_init()          ← called before printk
  └─ fb_init()
       └─ vmsvga_probe() → success: _drv = &vmsvga_drv
          (or bochs_probe() if vmsvga absent)
  └─ fb_get_char_dims(&max_col, &max_row)  ← per-TTY
  └─ tty_default_emit_unsafe registered as printk callback

printk("hello\n")
  └─ tty_lock_acquire / tty_default_emit_unsafe / tty_lock_release
       └─ process_one_char → vga_putchar → fb_putcell
            └─ _drv->putcell → render_cell (+ SVGA_CMD_UPDATE for vmsvga)

tty_switch(n)
  └─ fb_redraw(ttys[n].cells, cols, rows, cursor)
       └─ _drv->redraw  ← full repaint of new TTY's cell buffer
```
