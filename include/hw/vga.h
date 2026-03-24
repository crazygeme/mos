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

/* Lifecycle */

void fb_init();
void fb_enable();
void fb_map_lfb();

/* Query */

/* Fill *cols and *rows with the character grid dimensions. */
void fb_get_char_dims(unsigned *cols, unsigned *rows);

/* Drawing primitives */

void fb_write_color(int x, int y, unsigned color);

/* Character cell read/write (also updates the internal text shadow buffer). */
char fb_getchar(int col, int row);
void fb_putchar(int col, int row, char c);

/* Cursor */

/* Move the visible cursor to new_pos (linear char index). */
void fb_update_cursor(unsigned new_pos);

/* Screen operations */

/* Scroll the entire screen up by one character row. */
void fb_scroll_line(void);
void fb_scroll_region(unsigned top_row, unsigned bot_row);
void fb_insert_lines(unsigned row, unsigned bot_row, unsigned n);
void fb_delete_lines(unsigned row, unsigned bot_row, unsigned n);

/* Clear the entire framebuffer to black. */
void fb_clear_screen(void);

/* Save the text shadow buffer to dst (size = cols*rows bytes). */
void fb_save_text(char *dst, unsigned size);

/* Restore the text shadow buffer from src, redraw the entire screen,
 * and place the cursor at cursor_pos. */
void fb_restore_screen(const char *src, unsigned size, unsigned cursor_pos);

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
#define VGA_COLOR_BLUE ARGB(0xff, 0x00, 0x00, 0xFF)
#define VGA_COLOR_GRAY ARGB(0xff, 0xaa, 0xaa, 0xaa)
#define VGA_COLOR_YELLOW ARGB(0xff, 0xFF, 0xFF, 0x00)

/* Font cell dimensions in pixels. */
#define char_height 12
#define char_width 8

#endif /* BOOT_VIDEO_H */
