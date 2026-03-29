/** -*- linux-c -*- ------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2.
 *
 */

/**
 * Header file for the real-mode video probing code
 */

#ifndef _HW_BOOT_VIDEO_H
#define _HW_BOOT_VIDEO_H

#include <boot/multiboot.h>
#include <mm/mm.h>
#include <config.h>

typedef struct {
	char ch;
	unsigned fg;
	unsigned bg;
} tty_cell_t;

/* Lifecycle */
void fb_init(void);
void fb_get_char_dims(unsigned *cols, unsigned *rows);

/* Render one cell to the hardware framebuffer */
void fb_putcell(const tty_cell_t *cell, int col, int row);

/* Repaint the entire screen from a cell buffer and place the cursor */
void fb_redraw(const tty_cell_t *cells, unsigned cols, unsigned rows,
	       unsigned cursor_pos);

/* Move the visible cursor: restore cell at old_pos, draw cursor at new_pos */
void fb_cursor_update(unsigned old_pos, unsigned new_pos,
		      const tty_cell_t *cells, unsigned cols);

/* Pixel-only scroll/insert/delete — cell buffer is managed by tty.c */
void fb_scroll_line_px(void);
void fb_scroll_region_px(unsigned top_row, unsigned bot_row);
void fb_insert_lines_px(unsigned row, unsigned bot_row, unsigned n);
void fb_delete_lines_px(unsigned row, unsigned bot_row, unsigned n);
void fb_clear_screen(void);

/* Change font */
void fb_change_font(const char *name);

int fb_is_char_visiable(unsigned char c);

/* Erase the cursor underline at pos before a pixel-level scroll */
void fb_cursor_erase(unsigned pos, const tty_cell_t *cells, unsigned cols);

/* BGA / VBE register constants */
#define VBE_DISPI_ID5 0xB0C5
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF
#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED 0x01
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM 0x80
#define VBE_DISPI_INDEX_ID 0
#define VBE_DISPI_INDEX_XRES 1
#define VBE_DISPI_INDEX_YRES 2
#define VBE_DISPI_INDEX_BPP 3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_INDEX_BANK 5
#define VBE_DISPI_INDEX_VIRT_WIDTH 6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET 8
#define VBE_DISPI_INDEX_Y_OFFSET 9

#define VBE_DISPI_BPP_4 0x04
#define VBE_DISPI_BPP_8 0x08
#define VBE_DISPI_BPP_15 0x0F
#define VBE_DISPI_BPP_16 0x10
#define VBE_DISPI_BPP_24 0x18
#define VBE_DISPI_BPP_32 0x20

/* Colour helpers */
#define _RED(color) ((color & 0x00FF0000) / 0x10000)
#define _GRE(color) ((color & 0x0000FF00) / 0x100)
#define _BLU(color) ((color & 0x000000FF) / 0x1)
#define _ALP(color) ((color & 0xFF000000) / 0x1000000)

#define ARGB(a, r, g, b)                                  \
	((0xFF * 0x1000000ULL) + ((0xFF & r) * 0x10000) + \
	 ((0xFF & g) * 0x100) + ((0xFF & b) * 0x1))

#define VGA_COLOR_BLACK ARGB(0xff, 0x00, 0x00, 0x00)
#define VGA_COLOR_WHITE ARGB(0xff, 0xff, 0xff, 0xff)
#define VGA_COLOR_RED ARGB(0xff, 0xff, 0x00, 0x00)
#define VGA_COLOR_GREEN ARGB(0xff, 0x00, 0xff, 0x00)
#define VGA_COLOR_BLUE ARGB(0xff, 0x00, 0x00, 0xff)
#define VGA_COLOR_GRAY ARGB(0xff, 0xaa, 0xaa, 0xaa)
#define VGA_COLOR_YELLOW ARGB(0xff, 0xff, 0xff, 0x00)
#define VGA_COLOR_CYAN ARGB(0xff, 0x00, 0xff, 0xff)
#define VGA_COLOR_MAGENTA ARGB(0xff, 0xff, 0x00, 0xff)

#endif /* _HW_BOOT_VIDEO_H */
