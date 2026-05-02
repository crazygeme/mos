#include <lib/klib.h>
#include <errno.h>
#include <config.h>
#include <hw/pci.h>
#include <hw/vga.h>
#include <lib/port.h>

#define MOS_VM86_UNKNOWN 1
#define MOS_VM86_ENTER 1
#define MOS_VM86_ENTER_NO_BYPASS 2

#define MOS_VM86_HLT_CS 0x0000
#define MOS_VM86_HLT_IP 0x0600

#define MOS_VBE_FUNC_GET_INFO 0x4f00
#define MOS_VBE_FUNC_GET_MODE_INFO 0x4f01
#define MOS_VBE_FUNC_SET_MODE 0x4f02
#define MOS_VBE_FUNC_GET_MODE 0x4f03
#define MOS_VBE_FUNC_SAVE_RESTORE_STATE 0x4f04
#define MOS_VBE_FUNC_LOGICAL_SCANLINE 0x4f06
#define MOS_VBE_FUNC_DISPLAY_START 0x4f07
#define MOS_VBE_FUNC_DAC_PALETTE_FORMAT 0x4f08
#define MOS_VBE_FUNC_PALETTE 0x4f09
#define MOS_VBE_FUNC_SET_GET_PIXEL_CLOCK 0x4f0b
#define MOS_VBE_FUNC_DDC 0x4f15

#define MOS_VBE_LINEAR_FB 0x4000

#define VMSVGA_VENDOR_ID 0x15AD
#define VMSVGA_DEVICE_ID 0x0405

#define SVGA_INDEX_PORT 0
#define SVGA_VALUE_PORT 1

#define SVGA_REG_ID 0
#define SVGA_REG_ENABLE 1
#define SVGA_REG_WIDTH 2
#define SVGA_REG_HEIGHT 3
#define SVGA_REG_BPP 7
#define SVGA_REG_CONFIG_DONE 20

#define SVGA_ID_2 (0x900000UL << 8 | 2)

struct mos_vm86_regs {
	unsigned ebx;
	unsigned ecx;
	unsigned edx;
	unsigned esi;
	unsigned edi;
	unsigned ebp;
	unsigned eax;
	unsigned __null_ds;
	unsigned __null_es;
	unsigned __null_fs;
	unsigned __null_gs;
	unsigned orig_eax;
	unsigned eip;
	unsigned short cs, __csh;
	unsigned eflags;
	unsigned esp;
	unsigned short ss, __ssh;
	unsigned short es, __esh;
	unsigned short ds, __dsh;
	unsigned short fs, __fsh;
	unsigned short gs, __gsh;
} __attribute__((packed));

struct mos_revectored_struct {
	unsigned long map[8];
} __attribute__((packed));

struct mos_vm86_struct {
	struct mos_vm86_regs regs;
	unsigned long flags;
	unsigned long screen_bitmap;
	unsigned long cpu_type;
	struct mos_revectored_struct int_revectored;
	struct mos_revectored_struct int21_revectored;
} __attribute__((packed));

struct mos_vbe_info_block {
	char signature[4];
	unsigned short version;
	unsigned oem_string_ptr;
	unsigned capabilities;
	unsigned mode_list_ptr;
	unsigned short total_memory_64k;
	unsigned short oem_software_rev;
	unsigned oem_vendor_name_ptr;
	unsigned oem_product_name_ptr;
	unsigned oem_product_rev_ptr;
	unsigned char reserved[222];
	char oem_data[256];
} __attribute__((packed));

struct mos_vbe_mode_info {
	unsigned short attributes;
	unsigned char winA;
	unsigned char winB;
	unsigned short granularity;
	unsigned short winsize;
	unsigned short segmentA;
	unsigned short segmentB;
	unsigned realFctPtr;
	unsigned short pitch;
	unsigned short xres;
	unsigned short yres;
	unsigned char wchar;
	unsigned char ychar;
	unsigned char planes;
	unsigned char bpp;
	unsigned char banks;
	unsigned char memory_model;
	unsigned char bank_size;
	unsigned char image_pages;
	unsigned char reserved0;
	unsigned char red_mask;
	unsigned char red_position;
	unsigned char green_mask;
	unsigned char green_position;
	unsigned char blue_mask;
	unsigned char blue_position;
	unsigned char rsv_mask;
	unsigned char rsv_position;
	unsigned char directcolor_attributes;
	unsigned physbase;
	unsigned offscreen_mem_offset;
	unsigned short offscreen_mem_size;
	unsigned short lin_pitch;
	unsigned char bnk_image_pages;
	unsigned char lin_image_pages;
	unsigned char lin_red_mask;
	unsigned char lin_red_position;
	unsigned char lin_green_mask;
	unsigned char lin_green_position;
	unsigned char lin_blue_mask;
	unsigned char lin_blue_position;
	unsigned char lin_rsv_mask;
	unsigned char lin_rsv_position;
	unsigned max_pixel_clock;
	unsigned char reserved1[190];
} __attribute__((packed));

typedef struct {
	unsigned iobase;
	unsigned fb_phys;
	unsigned fb_size;
} mos_vbe_hw_t;

typedef struct {
	unsigned short mode_id;
	unsigned short width;
	unsigned short height;
	unsigned char bpp;
} mos_vbe_mode_t;

static const mos_vbe_mode_t mos_vbe_modes[] = {
	{ 0x111, 640, 480, 32 },
	{ 0x114, 800, 600, 32 },
	{ 0x117, 1024, 768, 32 },
	{ 0x11b, 1280, 1024, 32 },
};

static unsigned short mos_vbe_current_mode = 0x111;

static void mos_vbe_scan_pci(uint32_t device, uint16_t v, uint16_t d,
			     void *extra)
{
	if (v == VMSVGA_VENDOR_ID && d == VMSVGA_DEVICE_ID) {
		mos_vbe_hw_t *hw = (mos_vbe_hw_t *)extra;
		hw->iobase = pci_read_field(device, PCI_BAR0, 4) & ~3u;
		hw->fb_phys = pci_read_field(device, PCI_BAR1, 4) & ~15u;
		hw->fb_size = 16 * 1024 * 1024u;
	}
}

static mos_vbe_hw_t mos_vbe_probe_hw(void)
{
	mos_vbe_hw_t hw = { 0 };

	pci_scan(mos_vbe_scan_pci, -1, &hw);
	if (!hw.fb_phys)
		hw.fb_phys = 0xfd000000u;
	if (!hw.fb_size)
		hw.fb_size = 16 * 1024 * 1024u;
	return hw;
}

static void *mos_vm86_ptr(unsigned short seg, unsigned off)
{
	return (void *)(((unsigned)seg << 4) + (off & 0xffffu));
}

static unsigned mos_vm86_far_ptr(unsigned linear)
{
	return ((linear >> 4) << 16) | (linear & 0x000fu);
}

static void mos_vm86_finish(struct mos_vm86_struct *vm, unsigned eax)
{
	unsigned sp = vm->regs.esp & 0xffffu;

	vm->regs.eax = eax;
	vm->regs.cs = MOS_VM86_HLT_CS;
	vm->regs.eip = MOS_VM86_HLT_IP;
	vm->regs.esp = (vm->regs.esp & 0xffff0000u) | ((sp + 6) & 0xffffu);
}

static const mos_vbe_mode_t *mos_vbe_find_mode(unsigned mode_id)
{
	unsigned i;

	for (i = 0; i < sizeof(mos_vbe_modes) / sizeof(mos_vbe_modes[0]); i++) {
		if (mos_vbe_modes[i].mode_id == mode_id)
			return &mos_vbe_modes[i];
	}

	return NULL;
}

static const mos_vbe_mode_t *mos_vbe_find_mode_bx(unsigned bx)
{
	/*
	 * VBE mode set requests may carry control bits in the high bits of BX
	 * (linear framebuffer, preserve display memory, use CRTC timing, etc.).
	 * Match the low 9-bit mode number so calls like 0xc914 resolve to 0x114.
	 */
	return mos_vbe_find_mode(bx & 0x1ffu);
}

static unsigned short mos_vbe_default_mode(void)
{
	unsigned i;

	for (i = 0; i < sizeof(mos_vbe_modes) / sizeof(mos_vbe_modes[0]); i++) {
		if (mos_vbe_modes[i].width == VGA_RESOLUTION_X &&
		    mos_vbe_modes[i].height == VGA_RESOLUTION_Y &&
		    mos_vbe_modes[i].bpp == VGA_COLOR_DEPTH)
			return mos_vbe_modes[i].mode_id;
	}

	return mos_vbe_modes[0].mode_id;
}

static const mos_vbe_mode_t *mos_vbe_current_mode_info(void)
{
	const mos_vbe_mode_t *mode = mos_vbe_find_mode(mos_vbe_current_mode);

	if (!mode)
		mode = mos_vbe_find_mode(mos_vbe_default_mode());
	return mode ? mode : &mos_vbe_modes[0];
}

static void mos_vbe_hw_set_mode(const mos_vbe_mode_t *mode)
{
	mos_vbe_hw_t hw;

	if (!mode)
		return;

	hw = mos_vbe_probe_hw();
	if (!hw.iobase)
		return;

	port_write_dword(hw.iobase + SVGA_INDEX_PORT, SVGA_REG_ID);
	port_write_dword(hw.iobase + SVGA_VALUE_PORT, SVGA_ID_2);
	port_write_dword(hw.iobase + SVGA_INDEX_PORT, SVGA_REG_WIDTH);
	port_write_dword(hw.iobase + SVGA_VALUE_PORT, mode->width);
	port_write_dword(hw.iobase + SVGA_INDEX_PORT, SVGA_REG_HEIGHT);
	port_write_dword(hw.iobase + SVGA_VALUE_PORT, mode->height);
	port_write_dword(hw.iobase + SVGA_INDEX_PORT, SVGA_REG_BPP);
	port_write_dword(hw.iobase + SVGA_VALUE_PORT, mode->bpp);
	port_write_dword(hw.iobase + SVGA_INDEX_PORT, SVGA_REG_ENABLE);
	port_write_dword(hw.iobase + SVGA_VALUE_PORT, 1);
	port_write_dword(hw.iobase + SVGA_INDEX_PORT, SVGA_REG_CONFIG_DONE);
	port_write_dword(hw.iobase + SVGA_VALUE_PORT, 1);
}

static int mos_vm86_vbe_get_info(struct mos_vm86_struct *vm)
{
	struct mos_vbe_info_block *info;
	unsigned short *modes;
	char *oem;
	unsigned info_addr;
	unsigned mode_list_addr;
	unsigned oem_addr;
	unsigned i;

	info = (struct mos_vbe_info_block *)mos_vm86_ptr(vm->regs.es,
							 vm->regs.edi);
	if (!info)
		return 0;

	memset(info, 0, sizeof(*info));
	memcpy(info->signature, "VESA", 4);
	info->version = 0x0300;
	info->capabilities = 0;
	info->total_memory_64k = 0x0100;

	info_addr = ((unsigned)vm->regs.es << 4) + (vm->regs.edi & 0xffffu);
	oem_addr = info_addr + 0x100;
	mode_list_addr = info_addr + 0x180;
	oem = (char *)info + 0x100;
	strcpy(oem, "MOS VBE");
	modes = (unsigned short *)((char *)info + 0x180);
	for (i = 0; i < sizeof(mos_vbe_modes) / sizeof(mos_vbe_modes[0]); i++)
		modes[i] = mos_vbe_modes[i].mode_id;
	modes[i] = 0xffff;
	info->mode_list_ptr = mos_vm86_far_ptr(mode_list_addr);

	info->oem_string_ptr = mos_vm86_far_ptr(oem_addr);
	info->oem_vendor_name_ptr = info->oem_string_ptr;
	info->oem_product_name_ptr = info->oem_string_ptr;
	info->oem_product_rev_ptr = info->oem_string_ptr;

	mos_vm86_finish(vm, 0x004f);
	return 1;
}

static int mos_vm86_vbe_get_mode_info(struct mos_vm86_struct *vm)
{
	struct mos_vbe_mode_info *info;
	const mos_vbe_mode_t *mode;
	mos_vbe_hw_t hw;
	unsigned pitch;
	unsigned pages;
	unsigned visible_bytes;
	unsigned offscreen_bytes;

	mode = mos_vbe_find_mode(vm->regs.ecx & 0x3fffu);
	if (!mode)
		return 0;

	info = (struct mos_vbe_mode_info *)mos_vm86_ptr(vm->regs.es,
							vm->regs.edi);
	if (!info)
		return 0;

	hw = mos_vbe_probe_hw();
	pitch = mode->width * (mode->bpp / 8);
	pages = hw.fb_size / (pitch * mode->height);
	if (pages)
		pages--;
	visible_bytes = pitch * mode->height;
	offscreen_bytes =
		hw.fb_size > visible_bytes ? hw.fb_size - visible_bytes : 0;

	memset(info, 0, sizeof(*info));
	/*
	 * Present these as normal VBE 2.0+ packed-pixel graphics modes with
	 * BIOS output support, VGA compatibility, and a linear framebuffer.
	 * Old XFree86 vesa code is picky about these capability bits when
	 * deciding whether to use the LFB path or fall back to banked VGA.
	 */
	info->attributes = 0x00bf;
	info->winA = 0x07;
	info->winB = 0x00;
	info->granularity = 64;
	info->winsize = 64;
	info->segmentA = 0xa000;
	info->segmentB = 0xa000;
	info->pitch = pitch;
	info->xres = mode->width;
	info->yres = mode->height;
	info->wchar = 8;
	info->ychar = 16;
	info->planes = 1;
	info->bpp = mode->bpp;
	info->banks = 1;
	info->memory_model = 6;
	info->bank_size = 64;
	info->image_pages = pages > 255 ? 255 : pages;
	info->red_mask = 8;
	info->red_position = 16;
	info->green_mask = 8;
	info->green_position = 8;
	info->blue_mask = 8;
	info->blue_position = 0;
	info->rsv_mask = 8;
	info->rsv_position = 24;
	/*
	 * 32bpp x8r8g8b8-style direct color: programmable color ramp and the
	 * high 8 "reserved" bits are usable by the application.
	 */
	info->directcolor_attributes = 0x03;
	info->physbase = hw.fb_phys;
	info->offscreen_mem_offset = visible_bytes;
	info->offscreen_mem_size = offscreen_bytes / 1024;
	info->lin_pitch = pitch;
	info->bnk_image_pages = pages > 255 ? 255 : pages;
	info->lin_image_pages = pages > 255 ? 255 : pages;
	info->lin_red_mask = info->red_mask;
	info->lin_red_position = info->red_position;
	info->lin_green_mask = info->green_mask;
	info->lin_green_position = info->green_position;
	info->lin_blue_mask = info->blue_mask;
	info->lin_blue_position = info->blue_position;
	info->lin_rsv_mask = info->rsv_mask;
	info->lin_rsv_position = info->rsv_position;
	info->max_pixel_clock = 230000000;
	mos_vm86_finish(vm, 0x004f);
	return 1;
}

static int mos_vm86_vbe_logical_scanline(struct mos_vm86_struct *vm)
{
	const mos_vbe_mode_t *mode = mos_vbe_current_mode_info();
	mos_vbe_hw_t hw = mos_vbe_probe_hw();
	unsigned pitch = mode->width * (mode->bpp / 8);
	unsigned max_lines = pitch ? hw.fb_size / pitch : 0;

	/*
	 * XFree86 queries or tweaks the logical scanline after programming the
	 * mode. We only support the native pitch for the active mode, so answer
	 * with that fixed geometry for all supported subfunctions.
	 */
	switch (vm->regs.ebx & 0xffu) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
		vm->regs.ebx = (vm->regs.ebx & 0xffff0000u) | (pitch & 0xffffu);
		vm->regs.ecx = (vm->regs.ecx & 0xffff0000u) |
			       (mode->width & 0xffffu);
		vm->regs.edx = (vm->regs.edx & 0xffff0000u) |
			       (max_lines & 0xffffu);
		mos_vm86_finish(vm, 0x004f);
		return 1;
	default:
		return 0;
	}
}

static int mos_vm86_vbe_dac_palette_format(struct mos_vm86_struct *vm)
{
	switch (vm->regs.ebx & 0xffu) {
	case 0x00: /* set DAC width */
	case 0x01: /* get DAC width */
		vm->regs.ebx = (vm->regs.ebx & 0xffff00ffu) | (8u << 8);
		mos_vm86_finish(vm, 0x004f);
		return 1;
	default:
		return 0;
	}
}

static int mos_vm86_vbe_set_mode(struct mos_vm86_struct *vm)
{
	const mos_vbe_mode_t *mode;

	mode = mos_vbe_find_mode_bx(vm->regs.ebx);
	if (!mode)
		return 0;

	mos_vbe_current_mode = mode->mode_id;
	mos_vbe_hw_set_mode(mode);
	fb_sync_mode();
	mos_vm86_finish(vm, 0x004f);
	return 1;
}

static int mos_vm86_vbe_save_restore_state(struct mos_vm86_struct *vm)
{
	/*
	 * VBE 4F04 uses DL to select query/save/restore. The query path
	 * returns the required buffer size in 64-byte blocks via BX.
	 * Old XFree86's libvbe allocates BX << 6 bytes directly, so leaving
	 * BX untouched feeds it garbage and corrupts its heap state later.
	 * We do not persist any real BIOS state, so one 64-byte block is
	 * enough for our compatibility no-op.
	 */
	if ((vm->regs.edx & 0xffu) == 0x00)
		vm->regs.ebx = (vm->regs.ebx & 0xffff0000u) | 0x0001u;
	mos_vm86_finish(vm, 0x004f);
	return 1;
}

static int mos_vm86_vbe_display_start(struct mos_vm86_struct *vm)
{
	mos_vm86_finish(vm, 0x004f);
	return 1;
}

static int mos_vm86_vbe_palette(struct mos_vm86_struct *vm)
{
	mos_vm86_finish(vm, 0x004f);
	return 1;
}

static int mos_vm86_vbe_get_mode(struct mos_vm86_struct *vm)
{
	vm->regs.ebx = mos_vbe_current_mode | MOS_VBE_LINEAR_FB;
	mos_vm86_finish(vm, 0x004f);
	return 1;
}

static int mos_vm86_vbe_pixel_clock(struct mos_vm86_struct *vm)
{
	const mos_vbe_mode_t *mode;

	mode = mos_vbe_find_mode(vm->regs.edx & 0x3fffu);
	if (!mode)
		return 0;

	/*
	 * XFree86 asks VBE 3.0 for a refresh-specific pixel clock before
	 * setting the selected mode. Accept the requested clock as-is so it
	 * can continue down the normal linear-framebuffer path.
	 */
	mos_vm86_finish(vm, 0x004f);
	return 1;
}

static int mos_vm86_vbe_ddc(struct mos_vm86_struct *vm)
{
	mos_vm86_finish(vm, 0x014f);
	return 1;
}

static int mos_vm86_emulate(struct mos_vm86_struct *vm)
{
	unsigned ax;

	if (!vm)
		return 0;

	ax = vm->regs.eax & 0xffffu;

	switch (ax) {
	case MOS_VBE_FUNC_GET_INFO:
		return mos_vm86_vbe_get_info(vm);
	case MOS_VBE_FUNC_GET_MODE_INFO:
		return mos_vm86_vbe_get_mode_info(vm);
	case MOS_VBE_FUNC_SET_MODE:
		return mos_vm86_vbe_set_mode(vm);
	case MOS_VBE_FUNC_SAVE_RESTORE_STATE:
		return mos_vm86_vbe_save_restore_state(vm);
	case MOS_VBE_FUNC_GET_MODE:
		return mos_vm86_vbe_get_mode(vm);
	case MOS_VBE_FUNC_LOGICAL_SCANLINE:
		return mos_vm86_vbe_logical_scanline(vm);
	case MOS_VBE_FUNC_DISPLAY_START:
		return mos_vm86_vbe_display_start(vm);
	case MOS_VBE_FUNC_DAC_PALETTE_FORMAT:
		return mos_vm86_vbe_dac_palette_format(vm);
	case MOS_VBE_FUNC_PALETTE:
		return mos_vm86_vbe_palette(vm);
	case MOS_VBE_FUNC_SET_GET_PIXEL_CLOCK:
		return mos_vm86_vbe_pixel_clock(vm);
	case MOS_VBE_FUNC_DDC:
		return mos_vm86_vbe_ddc(vm);
	default:
		return 0;
	}
}

int sys_vm86old(void *user_vm86)
{
	struct mos_vm86_struct *vm = (struct mos_vm86_struct *)user_vm86;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("vm86old(%x)\n", user_vm86);

	if (!user_vm86)
		return -EFAULT;

	if (!mos_vbe_current_mode)
		mos_vbe_current_mode = mos_vbe_default_mode();

	if (mos_vm86_emulate(vm))
		return MOS_VM86_UNKNOWN;

	return MOS_VM86_UNKNOWN;
}

int sys_vm86(unsigned long fn, void *user_vm86plus)
{
	if (TEST_LOG(TEST_LOG_INFO))
		klog("vm86(fn=%x, %x)\n", fn, user_vm86plus);

	if (!user_vm86plus)
		return -EFAULT;

	if (fn != MOS_VM86_ENTER && fn != MOS_VM86_ENTER_NO_BYPASS)
		return -EINVAL;

	return sys_vm86old(user_vm86plus);
}
