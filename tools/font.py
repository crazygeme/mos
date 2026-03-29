#!/usr/bin/env python3
"""Convert a TTF font to Linux kernel font_8x16.c bitmap format (first 256 chars)."""

import sys
import os
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

TTF_PATH = sys.argv[1]
OUTPUT   =sys.argv[2]
NUM_CHARS = 256

# ── Read metrics from the font file ──────────────────────────────────────────
tt = TTFont(TTF_PATH)
upm        = tt['head'].unitsPerEm
ascent     = tt['hhea'].ascent          # positive, above baseline
descent    = -tt['hhea'].descent        # make positive (descent is negative)
adv_width  = tt['hmtx'].metrics['A'][0] # monospace: all glyphs share same advance

cell_h_units = ascent + descent         # total cell height in font units
cell_w_units = adv_width

# Choose a bitmap height that keeps integer width.
# Walk candidate heights (16..64) and pick the smallest that gives integer width.
def best_size(w_units, h_units):
    ratio = w_units / h_units           # e.g. 0.5  → width is half height
    for h in range(16, 65):             # start at 16 — minimum usable height
        w = h * ratio
        if abs(w - round(w)) < 1e-6 and round(w) >= 4:
            return int(round(w)), h
    # fallback: round to nearest integers
    for h in range(16, 65):
        w = round(h * ratio)
        if w >= 4:
            return int(w), h
    return 8, 16

CHAR_W, CHAR_H = best_size(cell_w_units, cell_h_units)
print(f"Font metrics: upm={upm}, ascent={ascent}, descent={descent}, "
      f"adv={adv_width}")
print(f"Bitmap cell: {CHAR_W}×{CHAR_H}")

# ── Compute PIL font size so the em fits the cell ────────────────────────────
# PIL size parameter = em height in pixels (at 72 DPI).
# We want ascent+descent == CHAR_H, so:
#   scale = CHAR_H / cell_h_units
#   pil_size = upm * scale  (em in pixels)
scale    = CHAR_H / cell_h_units
pil_size = max(4, round(upm * scale))
baseline = round(ascent * scale)        # pixel row of the baseline inside cell

pil_font = ImageFont.truetype(TTF_PATH, size=pil_size)

# ── Render each character ────────────────────────────────────────────────────
def render_char(cp: int) -> list[int]:
    """Return CHAR_H bytes, each byte = one row of CHAR_W pixels (MSB = left)."""
    img = Image.new("L", (CHAR_W, CHAR_H), 0)
    draw = ImageDraw.Draw(img)

    ch = chr(cp)
    # getbbox returns (left, top, right, bottom) relative to the anchor point
    bbox = pil_font.getbbox(ch)
    if bbox is None:
        return [0] * CHAR_H

    # Draw with 'ls' (left-side bearing, baseline) anchor
    draw.text((0, baseline), ch, font=pil_font, fill=255, anchor="ls")

    # Threshold and pack into bytes (MSB = leftmost pixel)
    rows = []
    pixels = img.load()
    for y in range(CHAR_H):
        byte = 0
        for x in range(CHAR_W):
            if pixels[x, y] > 127:
                byte |= (1 << (CHAR_W - 1 - x))
        rows.append(byte)
    return rows

# ── Format helpers ────────────────────────────────────────────────────────────
def char_label(cp: int) -> str:
    ch = chr(cp)
    if ch.isprintable() and ch not in ("'", "\\"):
        return f"'{ch}'"
    return f"0x{cp:02X}"

def bitmap_comment(byte: int, width: int) -> str:
    bits = bin(byte)[2:].zfill(width)
    return bits.replace('0', '.').replace('1', '#')

# ── Write C file ─────────────────────────────────────────────────────────────
FONTDATAMAX = NUM_CHARS * CHAR_H

font_name = os.path.os.path.basename(OUTPUT)
font_name = os.path.splitext(font_name)[0]

with open(OUTPUT, "w") as f:
    f.write(f"""\
// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated from FiraCodeNerdFont-Medium.ttf
 * Bitmap size: {CHAR_W}x{CHAR_H}, first {NUM_CHARS} characters
 */

#include <hw/font.h>
#include <macro.h>

#define FONTDATAMAX {FONTDATAMAX}

static const unsigned char _{font_name}_glyphs[] = {{
""")

    for cp in range(NUM_CHARS):
        label = char_label(cp)
        f.write(f"\t/* index 0x{cp:02X} {label} */\n")
        rows = render_char(cp)
        for i, byte in enumerate(rows):
            comment = bitmap_comment(byte, CHAR_W)
            comma = "," if (cp < NUM_CHARS - 1 or i < CHAR_H - 1) else " "
            f.write(f"\t0x{byte:02X}{comma} /* {comment} */\n")
        f.write("\n")

    f.write(f"""\
}};
            
static const unsigned char _{font_name}_cursor_glyphs[] = {{
""")
    show_width = int(CHAR_W / 4)
    f.write(f"\t/* index 0: underline cursor */\n")
    for i in range(CHAR_H - show_width):
        f.write(f"\t0x00,\n")
    for i in range(show_width):
        f.write(f"\t0xff,\n")
    f.write(f"\t/* index 1: blank */\n")
    for i in range(CHAR_H):
        f.write(f"\t0x00,\n")
    
    f.write(f"""\
}};

const fb_font_t font_{font_name} = {{
\t.name\t= "{font_name}",
\t.width\t= {CHAR_W},
\t.height\t= {CHAR_H},
\t.charcount= {NUM_CHARS},
\t.glyphs = _{font_name}_glyphs,
\t.cursor_glyphs = _{font_name}_cursor_glyphs
}};
""")

print(f"Written {OUTPUT}  ({NUM_CHARS} chars × {CHAR_H} rows = {FONTDATAMAX} bytes)")
