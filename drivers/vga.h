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

#include <int/int.h>
#include <lib/klib.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/**
 * This code uses an extended set of video mode numbers. These include:
 * Aliases for standard modes
 *      NORMAL_VGA (-1)
 *      EXTENDED_VGA (-2)
 *      ASK_VGA (-3)
 * Video modes numbered by menu position -- NOT RECOMMENDED because of lack
 * of compatibility when extending the table. These are between 0x00 and 0xff.
 */
#define VIDEO_FIRST_MENU 0x0000

/** Standard BIOS video modes (BIOS number + 0x0100) */
#define VIDEO_FIRST_BIOS 0x0100

/** VESA BIOS video modes (VESA number + 0x0200) */
#define VIDEO_FIRST_VESA 0x0200

/** Video7 special modes (BIOS number + 0x0900) */
#define VIDEO_FIRST_V7 0x0900

/** Special video modes */
#define VIDEO_FIRST_SPECIAL 0x0f00
#define VIDEO_80x25 0x0f00
#define VIDEO_8POINT 0x0f01
#define VIDEO_80x43 0x0f02
#define VIDEO_80x28 0x0f03
#define VIDEO_CURRENT_MODE 0x0f04
#define VIDEO_80x30 0x0f05
#define VIDEO_80x34 0x0f06
#define VIDEO_80x60 0x0f07
#define VIDEO_GFX_HACK 0x0f08
#define VIDEO_LAST_SPECIAL 0x0f09

/** Video modes given by resolution */
#define VIDEO_FIRST_RESOLUTION 0x1000

/** The "recalculate timings" flag */
#define VIDEO_RECALC 0x8000


/** Basic video information */
#define ADAPTER_CGA	0	/** CGA/MDA/HGC */
#define ADAPTER_EGA	1
#define ADAPTER_VGA	2



/** Accessing VGA indexed registers */
_START static inline u8 in_idx(u16 port, u8 index)
{
	write_port(port, index);
	return read_port(port+1);
}

_START static inline void out_idx(u8 v, u16 port, u8 index)
{
	write_word(port, index+(v << 8));
}

/** Writes a value to an indexed port and then reads the port again */
_START static inline u8 tst_idx(u8 v, u16 port, u8 index)
{
	out_idx(port, index, v);
	return in_idx(port, index);
}


_START void intcall(u8 int_no, const struct biosregs *ireg, struct biosregs *oreg);
_START int vga_set_mode(unsigned mode);

#endif /** BOOT_VIDEO_H */

