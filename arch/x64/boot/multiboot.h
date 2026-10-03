/* multiboot.h - the header for Multiboot */
/* Copyright (C) 1999, 2001  Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#ifndef _BOOT_MULTIBOOT_H_
#define _BOOT_MULTIBOOT_H_

/* Macros. */

/* The magic number for the Multiboot header. */
#define MULTIBOOT_HEADER_MAGIC 0x1BADB002

/* The flags for the Multiboot header. */
#ifdef __ELF__
#define MULTIBOOT_HEADER_FLAGS 0x00000003
#else
#define MULTIBOOT_HEADER_FLAGS 0x00010003
#endif

/* The magic number passed by a Multiboot-compliant boot loader. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* The size of our stack (16KB). */
#define STACK_SIZE 0x4000

/* C symbol format. HAVE_ASM_USCORE is defined by configure. */
#ifdef HAVE_ASM_USCORE
#define EXT_C(sym) _##sym
#else
#define EXT_C(sym) sym
#endif

#ifndef ASM
/* Do not include here in boot.S. */

/* Multiboot v1 wire values remain 32-bit in a 64-bit kernel. */
#include <stdint.h>

/* Types. */

/* The Multiboot header. */
typedef struct multiboot_header {
	uint32_t magic;
	uint32_t flags;
	uint32_t checksum;
	uint32_t header_addr;
	uint32_t load_addr;
	uint32_t load_end_addr;
	uint32_t bss_end_addr;
	uint32_t entry_addr;
} __attribute__((packed)) multiboot_header_t;

/* The symbol table for a.out. */
typedef struct aout_symbol_table {
	uint32_t tabsize;
	uint32_t strsize;
	uint32_t addr;
	uint32_t reserved;
} __attribute__((packed)) aout_symbol_table_t;

/* The section header table for ELF. */
typedef struct elf_section_header_table {
	uint32_t num;
	uint32_t size;
	uint32_t addr;
	uint32_t shndx;
} __attribute__((packed)) elf_section_header_table_t;

/* The Multiboot information. */
typedef struct multiboot_info {
	uint32_t flags;
	uint32_t mem_lower;
	uint32_t mem_upper;
	uint32_t boot_device;
	uint32_t cmdline;
	uint32_t mods_count;
	uint32_t mods_addr;
	union {
		aout_symbol_table_t aout_sym;
		elf_section_header_table_t elf_sec;
	} u;
	uint32_t mmap_length;
	uint32_t mmap_addr;
	uint32_t drives_length;
	uint32_t drives_addr;
	uint32_t config_table;
	uint32_t boot_loader_name;
	uint32_t apm_table;
	uint32_t vbe_control_info;
	uint32_t vbe_mode_info;
	uint16_t vbe_mode;
	uint16_t vbe_interface_seg;
	uint16_t vbe_interface_off;
	uint16_t vbe_interface_len;
} __attribute__((packed)) multiboot_info_t;
_Static_assert(sizeof(multiboot_info_t) == 88,
	       "Multiboot information wire size");

typedef struct {
	unsigned short attributes;
	unsigned char winA, winB;
	unsigned short granularity;
	unsigned short winsize;
	unsigned short segmentA, segmentB;
	unsigned int realFctPtr;
	unsigned short pitch;

	unsigned short Xres, Yres;
	unsigned char Wchar, Ychar, planes, bpp, banks;
	unsigned char memory_model, bank_size, image_pages;
	unsigned char reserved0;

	unsigned char red_mask, red_position;
	unsigned char green_mask, green_position;
	unsigned char blue_mask, blue_position;
	unsigned char rsv_mask, rsv_position;
	unsigned char directcolor_attributes;

	unsigned int physbase;
	unsigned int reserved1;
	unsigned short reserved2;
} __attribute__((packed)) vbe_info_t;

/* The module structure. */
typedef struct module {
	uint32_t mod_start;
	uint32_t mod_end;
	uint32_t string;
	uint32_t reserved;
} module_t;

/* The memory map. Be careful that the offset 0 is base_addr_low
   but no size. */
typedef struct memory_map {
	uint32_t size;
	uint32_t base_addr_low;
	uint32_t base_addr_high;
	uint32_t length_low;
	uint32_t length_high;
	uint32_t type;
} memory_map_t;

_Static_assert(sizeof(multiboot_header_t) == 32, "Multiboot header width");
_Static_assert(__builtin_offsetof(multiboot_info_t, cmdline) == 16,
	       "Multiboot command line offset");
_Static_assert(__builtin_offsetof(multiboot_info_t, mmap_length) == 44,
	       "Multiboot map length offset");
_Static_assert(__builtin_offsetof(multiboot_info_t, mmap_addr) == 48,
	       "Multiboot map address offset");
_Static_assert(sizeof(memory_map_t) == 24, "Multiboot memory record width");
#endif /* ! ASM */

#endif
