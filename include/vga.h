/** -*- linux-c -*- ------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2.
 *
 * ----------------------------------------------------------------------- */

/**
 * Header file for the real-mode video probing code
 */

#ifndef BOOT_VIDEO_H
#define BOOT_VIDEO_H

#include <multiboot.h>
#include <mm.h>
#include <config.h>



_START void fb_init(multiboot_info_t* mboot_ptr);


void fb_enable();

void fb_map_lfb();
/*
 * terminal bitmap fallback font
 */

/* Binary Literals */
#define b(x) ((unsigned char)b_(0 ## x ## uL))
#define b_(x) ((x & 1) | (x >> 2 & 2) | (x >> 4 & 4) | (x >> 6 & 8) | (x >> 8 & 16) | (x >> 10 & 32) | (x >> 12 & 64) | (x >> 14 & 128))

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


extern unsigned char ** _number_font;
extern unsigned _fb_buffer;
extern unsigned _resolution_x;
extern unsigned _resolution_y;
extern unsigned _hw_resolution_x;
extern unsigned _hw_resolution_y;
extern unsigned _window_char_width;
extern unsigned _window_char_height;
extern unsigned _fb_font_width;
extern unsigned _fb_font_height;
extern char _fb_text[];
extern unsigned _fb_x_off;
extern unsigned _fb_y_off;

void fb_write_char(int x, int y, int val, unsigned color);
void fb_write_color(int x, int y, unsigned color);
void fb_set_point(int x, int y, unsigned value);

// pre defined color
#define _RED(color) ((color & 0x00FF0000) / 0x10000)
#define _GRE(color) ((color & 0x0000FF00) / 0x100)
#define _BLU(color) ((color & 0x000000FF) / 0x1)
#define _ALP(color) ((color & 0xFF000000) / 0x1000000)

#define ARGB(a, r, g, b) ( (0xFF *0x1000000ULL) + ((0xFF & r) * 0x10000) + ((0xFF & g) * 0x100) + ((0xFF & b) * 0x1))
#define VGA_COLOR_BLACK ARGB(0xff, 0x00, 0x00, 0x00)
#define VGA_COLOR_WHITE ARGB(0xff, 0xff, 0xff, 0xff)
#define VGA_COLOR_RED ARGB(0xff, 0xff, 0x00, 0x00)
#define VGA_COLOR_GREEN ARGB(0xff, 0x00, 0xff, 0x00)
#define VGA_COLOR_BLUE ARGB(0xff, 0x00, 0x00, 0xFF)
#define VGA_COLOR_GRAY ARGB(0xff, 0xaa, 0xaa, 0xaa)
#define VGA_COLOR_YELLOW ARGB(0xff, 0xFF, 0xFF, 0x00)

#define char_height 12
#define char_width  8

#endif /** BOOT_VIDEO_H */
