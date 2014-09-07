/** -*- linux-c -*- ------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *   Copyright 2009 Intel Corporation; author H. Peter Anvin
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2.
 *
 * ----------------------------------------------------------------------- */

/**
 * Common all-VGA modes
 */

#include <drivers/vga.h>
#define outb(d, p) write_port(p, d)
#define inb read_port

struct biosregs {
	union {
		struct {
			u32 edi;
			u32 esi;
			u32 ebp;
			u32 _esp;
			u32 ebx;
			u32 edx;
			u32 ecx;
			u32 eax;
			u32 _fsgs;
			u32 _dses;
			u32 eflags;
		};
		struct {
			u16 di, hdi;
			u16 si, hsi;
			u16 bp, hbp;
			u16 _sp, _hsp;
			u16 bx, hbx;
			u16 dx, hdx;
			u16 cx, hcx;
			u16 ax, hax;
			u16 gs, fs;
			u16 es, ds;
			u16 flags, hflags;
		};
		struct {
			u8 dil, dih, edi2, edi3;
			u8 sil, sih, esi2, esi3;
			u8 bpl, bph, ebp2, ebp3;
			u8 _spl, _sph, _esp2, _esp3;
			u8 bl, bh, ebx2, ebx3;
			u8 dl, dh, edx2, edx3;
			u8 cl, ch, ecx2, ecx3;
			u8 al, ah, eax2, eax3;
		};
	};
};

typedef void (*fpmemset)(void* addr, char val, int len);
_STARTDATA static fpmemset _memset = (fpmemset)((unsigned)memset - KERNEL_OFFSET);

_START static void initregs(struct biosregs *reg)
{
          _memset(reg, 0, sizeof *reg);
          reg->eflags |= 0;
          __asm__("mov %%ds, %0" : "=m"(reg->ds));
          __asm__("mov %%ds, %0" : "=m"(reg->es));
          __asm__("mov %%fs, %0" : "=m"(reg->fs));
          __asm__("mov %%gs, %0" : "=m"(reg->gs));
//        reg->ds = ds();
//        reg->es = ds();
//        reg->fs = fs();
//        reg->gs = gs();
}


/** Set basic 80x25 mode */
_START static u8 vga_set_basic_mode(void)
{
	struct biosregs ireg, oreg;
	u8 mode;

	initregs(&ireg);

	/** Query current mode */
	ireg.ax = 0x0f00;
	intcall(0x10, &ireg, &oreg);
	mode = oreg.al;

	if (mode != 3 && mode != 7)
		mode = 3;

	/** Set the mode */
	ireg.ax = mode;		/** AH=0: set mode */
	intcall(0x10, &ireg, NULL);
	return mode;
}

_START static void vga_set_8font(void)
{
	/** Set 8x8 font - 80x43 on EGA, 80x50 on VGA */
	struct biosregs ireg;

	initregs(&ireg);

	/** Set 8x8 font */
	ireg.ax = 0x1112;
	/** ireg.bl = 0; */
	intcall(0x10, &ireg, NULL);

	/** Use alternate print screen */
	ireg.ax = 0x1200;
	ireg.bl = 0x20;
	intcall(0x10, &ireg, NULL);

	/** Turn off cursor emulation */
	ireg.ax = 0x1201;
	ireg.bl = 0x34;
	intcall(0x10, &ireg, NULL);

	/** Cursor is scan lines 6-7 */
	ireg.ax = 0x0100;
	ireg.cx = 0x0607;
	intcall(0x10, &ireg, NULL);
}

_START static void vga_set_14font(void)
{
	/** Set 9x14 font - 80x28 on VGA */
	struct biosregs ireg;

	initregs(&ireg);

	/** Set 9x14 font */
	ireg.ax = 0x1111;
	/** ireg.bl = 0; */
	intcall(0x10, &ireg, NULL);

	/** Turn off cursor emulation */
	ireg.ax = 0x1201;
	ireg.bl = 0x34;
	intcall(0x10, &ireg, NULL);

	/** Cursor is scan lines 11-12 */
	ireg.ax = 0x0100;
	ireg.cx = 0x0b0c;
	intcall(0x10, &ireg, NULL);
}

_START static void vga_set_80x43(void)
{
	/** Set 80x43 mode on VGA (not EGA) */
	struct biosregs ireg;

	initregs(&ireg);

	/** Set 350 scans */
	ireg.ax = 0x1201;
	ireg.bl = 0x30;
	intcall(0x10, &ireg, NULL);

	/** Reset video mode */
	ireg.ax = 0x0003;
	intcall(0x10, &ireg, NULL);

	vga_set_8font();
}

/** I/O address of the VGA CRTC */
_START u16 vga_crtc(void)
{
	return (inb(0x3cc) & 1) ? 0x3d4 : 0x3b4;
}

_START static void vga_set_480_scanlines(void)
{
	u16 crtc;		/** CRTC base address */
	u8  csel;		/** CRTC miscellaneous output register */

	crtc = vga_crtc();

	out_idx(0x0c, crtc, 0x11); /** Vertical sync end, unlock CR0-7 */
	out_idx(0x0b, crtc, 0x06); /** Vertical total */
	out_idx(0x3e, crtc, 0x07); /** Vertical overflow */
	out_idx(0xea, crtc, 0x10); /** Vertical sync start */
	out_idx(0xdf, crtc, 0x12); /** Vertical display end */
	out_idx(0xe7, crtc, 0x15); /** Vertical blank start */
	out_idx(0x04, crtc, 0x16); /** Vertical blank end */
	csel = inb(0x3cc);
	csel &= 0x0d;
	csel |= 0xe2;
	outb(csel, 0x3c2);
}

_START static void vga_set_vertical_end(int lines)
{
	u16 crtc;		/** CRTC base address */
	u8  ovfw;		/** CRTC overflow register */
	int end = lines-1;

	crtc = vga_crtc();

	ovfw = 0x3c | ((end >> (8-1)) & 0x02) | ((end >> (9-6)) & 0x40);

	out_idx(ovfw, crtc, 0x07); /** Vertical overflow */
	out_idx(end,  crtc, 0x12); /** Vertical display end */
}

_START static void vga_set_80x30(void)
{
	vga_set_480_scanlines();
	vga_set_vertical_end(30*16);
}

_START static void vga_set_80x34(void)
{
	vga_set_480_scanlines();
	vga_set_14font();
	vga_set_vertical_end(34*14);
}

_START static void vga_set_80x60(void)
{
	vga_set_480_scanlines();
	vga_set_8font();
	vga_set_vertical_end(60*8);
}

_START int vga_set_mode(unsigned mode)
{
	/** Set the basic mode */
	vga_set_basic_mode();

	switch (mode) {
	case VIDEO_80x25:
		break;
	case VIDEO_8POINT:
		vga_set_8font();
		break;
	case VIDEO_80x43:
		vga_set_80x43();
		break;
	case VIDEO_80x28:
		vga_set_14font();
		break;
	case VIDEO_80x30:
		vga_set_80x30();
		break;
	case VIDEO_80x34:
		vga_set_80x34();
		break;
	case VIDEO_80x60:
		vga_set_80x60();
		break;
	}

	return 0;
}


