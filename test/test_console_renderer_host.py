"""Verify the production console renderer against a padded, replaceable surface."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <console/render.h>
#include <device/framebuffer.h>

static framebuffer_surface current;
static unsigned begin, end, presents, refreshes, mode_syncs;
static void surface(framebuffer_surface *out) { *out = current; }
static void damage(unsigned a, unsigned b) {
    assert(a < b && b <= current.pitch * current.height);
    if (!end || a < begin) begin = a;
    if (b > end) end = b;
}
static void present(void) { presents++; begin = end = 0; }
static void refresh(void) { refreshes++; }
static void sync_mode(void) { mode_syncs++; }
static const framebuffer_ops ops = {
    .surface = surface, .damage = damage, .present = present,
    .refresh = refresh, .sync_mode = sync_mode,
};
static void reset(unsigned char byte) {
    memset(current.pixels, byte, current.pitch * current.height);
    begin = end = 0;
}
static uint32_t pixel(unsigned x, unsigned y) {
    return current.pixels[y * (current.pitch / 4) + x];
}
int main(void) {
    unsigned cols, rows;
    console_get_char_dims(&cols, &rows);
    assert(cols == 0 && rows == 0 && framebuffer_available());
    current.width = 16; current.height = 48; current.pitch = 80;
    current.pixels = malloc(current.pitch * current.height);
    assert(current.pixels);
    framebuffer_register(&ops);
    assert(!framebuffer_available() && framebuffer_requires_refresh());
    console_get_char_dims(&cols, &rows);
    assert(cols == 2 && rows == 3);
    const console_font *font = font_get("vga16");
    console_cell cell = { 'A', 0x00112233, 0x00445566 };
    reset(0xa5);
    console_putcell(&cell, 1, 1);
    assert(begin == 16 * 80 + 8 * 4 && end == 31 * 80 + 16 * 4);
    for (unsigned y = 0; y < 48; y++) {
        for (unsigned x = 0; x < 20; x++) {
            uint32_t expected = 0xa5a5a5a5;
            if (y >= 16 && y < 32 && x >= 8 && x < 16)
                expected = (font->glyphs['A' * 16 + y - 16] & (128 >> (x - 8))) ? cell.fg : cell.bg;
            assert(pixel(x, y) == expected);
        }
    }
    // Invalid geometry cannot touch the buffer or submit damage.
    console_present();
    console_putcell(&cell, -1, 0); console_putcell(&cell, 2, 0);
    console_putcell(&cell, 0, 3); console_putcell(NULL, 0, 0);
    console_scroll_region(2, 1); console_insert_lines(0, 3, 1);
    console_delete_lines(0, 2, 0); console_cursor_erase(0, &cell, 0);
    assert(end == 0);
    // Cursor erase restores the same glyph, including its original colors.
    console_cell *cells = calloc(6, sizeof(*cells)); assert(cells);
    for (unsigned i = 0; i < 6; i++) cells[i] = cell;
    console_cursor_update(0, 3, cells, 2);
    unsigned cursor_pixels = 0;
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 8; x++)
            if (font->cursor_glyphs[y] & (128 >> x)) {
                assert(pixel(8 + x, 16 + y) == (uint32_t)VGA_COLOR_WHITE);
                cursor_pixels++;
            }
    assert(cursor_pixels);
    console_cursor_erase(3, cells, 2);
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 8; x++)
            assert(pixel(8 + x, 16 + y) == ((font->glyphs['A' * 16 + y] & (128 >> x)) ? cell.fg : cell.bg));
    // Scrolling, insertion and deletion move overlapping pixel rectangles.
    reset(0);
    for (unsigned y = 0; y < 48; y++)
        for (unsigned x = 0; x < 20; x++) current.pixels[y * 20 + x] = y / 16 + 1;
    console_scroll_region(0, 2);
    assert(pixel(0, 0) == 2 && pixel(0, 16) == 3 && pixel(0, 32) == 0);
    console_insert_lines(0, 2, 1);
    assert(pixel(0, 0) == 0 && pixel(0, 16) == 2 && pixel(0, 32) == 3);
    console_delete_lines(0, 2, 1);
    assert(pixel(0, 0) == 2 && pixel(0, 16) == 3 && pixel(0, 32) == 0);
    console_insert_lines(1, 2, 99);
    assert(pixel(0, 0) == 2 && pixel(0, 16) == 0 && pixel(0, 32) == 0);
    console_scroll_line(); assert(pixel(0, 0) == 0);
    // Mode changes may replace the pixel buffer; renderer must reacquire it.
    uint32_t *old = current.pixels;
    current.width = 8; current.height = 16; current.pitch = 32;
    current.pixels = calloc(1, current.pitch * current.height); assert(current.pixels);
    console_sync_mode(); assert(mode_syncs == 1);
    console_get_char_dims(&cols, &rows); assert(cols == 1 && rows == 1);
    console_redraw(cells, 1, 1, 99); // hidden/absent cursor
    assert(presents == 2 && end == 0);
    framebuffer_refresh(); assert(refreshes == 1);
    free(old); free(current.pixels); free(cells);
    return 0;
}
'''


class ConsoleRendererHost(unittest.TestCase):
    def test_pixels_cursor_scroll_and_mode_change(self):
        with tempfile.TemporaryDirectory(prefix="mos-console-") as tmp:
            path = Path(tmp)
            (path / "lib").mkdir()
            (path / "lib/klib.h").write_text(
                "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n"
                "#include <string.h>\n#include <ctype.h>\n"
            )
            probe = path / "probe.c"
            probe.write_text(PROBE)
            binary = path / "probe"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(path), "-I", str(ROOT / "src"),
                str(probe), str(ROOT / "src/console/render.c"),
                str(ROOT / "src/device/core/framebuffer.c"),
                str(ROOT / "src/console/fonts/font.c"),
                str(ROOT / "src/console/fonts/font_vga16.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)
