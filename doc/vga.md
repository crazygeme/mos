# Console rendering and display devices

Terminal semantics and display hardware have separate interfaces.

| Source | Responsibility |
| --- | --- |
| `src/driver/tty/tty.c` | ANSI/termios state, virtual terminals, character endpoints, graphics VT ownership |
| `src/console/render.c` | Font rasterization, text colors, cursor drawing, character geometry and text-row operations |
| `src/console/fonts` | Read-only built-in fonts |
| `src/device/core/framebuffer.c` | Pixel-device registration and dispatch, rectangle bounds and CPU fallbacks |
| `src/driver/video/bochs.c` | VBE registers, memory mapping, shadow surface and dirty-byte presentation |
| `src/driver/video/vmware_svga.c` | SVGA registers, VRAM mapping, FIFO updates and pixel copy/fill acceleration |

`console/render.h` defines `console_cell`, font data and rendering operations.
`device/framebuffer.h` defines an XRGB8888 pixel surface and `framebuffer_ops`.
The generic driver registration header contains no console or framebuffer types.

## Text output

The TTY driver maintains its cell buffer even when inactive. Active-terminal
writes call `console_putcell`; cursor movement calls `console_cursor_update`.
The renderer converts cells into pixels using the selected font and the surface's
byte pitch, then reports damaged byte ranges to the display device. A completed
write calls `console_present`, which submits the accumulated update. Kernel
console output also flushes at its output boundary through the console provider.

Text scrolling and line insertion/deletion translate row counts into pixel
rectangles. Display devices may accelerate rectangle copying and filling.
Otherwise the pixel core uses overlap-safe CPU operations without allocating a
scratch image. Neither concrete display driver knows the font or cell structure.

## Modes and terminal switching

Display probes register the pixel device through the ordinary PCI binding flow.
The virtual TTY probe then obtains character dimensions from the renderer.
`console_sync_mode` asks the display device to synchronize its live mode; the
renderer reacquires the surface because a resize may replace its pixel buffer.

Returning to a text VT redraws its saved cells using `console_redraw`.
Graphics VTs use pixel snapshots through the framebuffer service. The Bochs
snapshot reads physical scanout memory so userspace writes are preserved, and
restoration updates both scanout memory and its shadow surface. VMware restores
VRAM and submits a device update.

The periodic process service asks the registered console provider whether it
needs refresh. The TTY driver schedules graphics updates when the owner has
written to the mapped framebuffer. The process service has no display-specific
imports or calls.

The current surface format is XRGB8888 and the built-in font is VGA 8x16.
Other pixel formats and multiple active displays are not implemented.
