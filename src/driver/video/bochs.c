#include <int/int.h>
#include <boot/multiboot.h>
#include <driver/driver.h>
#include <device/framebuffer.h>
#include <errno.h>
#include <driver/video/bochs.h>
#include <mm/mm.h>
#include <macro.h>
#include <mm/mmu.h>
#include <lib/port.h>
#include <lib/klib.h>

/* Bochs VBE / QEMU stdvga register interface */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF
#define VBE_DISPI_ID5 0xB0C5
#define VBE_DISPI_DISABLED 0x00
#define VBE_DISPI_ENABLED 0x01
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM 0x80
#define VBE_DISPI_INDEX_ID 0
#define VBE_DISPI_INDEX_XRES 1
#define VBE_DISPI_INDEX_YRES 2
#define VBE_DISPI_INDEX_BPP 3
#define VBE_DISPI_INDEX_ENABLE 4

static uintptr_t _fb_buffer;
static unsigned shadow_bytes, dirty_begin, dirty_end;
static unsigned _fb_phys;
static unsigned _fb_mapped_bytes;
static unsigned _hw_resolution_x;
static unsigned _hw_resolution_y;

static void bochs_dirty(unsigned begin, unsigned end)
{
	if (!dirty_end || begin < dirty_begin)
		dirty_begin = begin;
	if (end > dirty_end)
		dirty_end = end;
}

static void bochs_present(void)
{
	if (!dirty_end)
		return;
	memcpy((void *)(uintptr_t)(_fb_phys + dirty_begin),
	       (const void *)(_fb_buffer + dirty_begin),
	       dirty_end - dirty_begin);
	dirty_begin = dirty_end = 0;
}

static int bochs_resize_shadow(unsigned width, unsigned height)
{
	unsigned bytes = width * height * (VGA_COLOR_DEPTH / 8);
	unsigned pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	if (pages * PAGE_SIZE > shadow_bytes) {
		vaddr_t buffer = vm_alloc(pages);
		if (!buffer)
			return 0;
		if (_fb_buffer)
			vm_free(_fb_buffer, shadow_bytes / PAGE_SIZE);
		_fb_buffer = buffer;
		shadow_bytes = pages * PAGE_SIZE;
	}
	memset((void *)_fb_buffer, 0, bytes);
	dirty_begin = dirty_end = 0;
	return 1;
}

static void bochs_ensure_fb_mapping(unsigned width, unsigned height)
{
	unsigned need_bytes;
	unsigned a;

	if (width == 0 || height == 0)
		return;

	need_bytes = width * height * (VGA_COLOR_DEPTH / 8);
	if (need_bytes <= _fb_mapped_bytes)
		return;

	for (a = _fb_phys + _fb_mapped_bytes; a < _fb_phys + need_bytes;
	     a += PAGE_SIZE)
		if (mm_map_io(a) == 1)
			arch_mm_invalidate(a);
	_fb_mapped_bytes = need_bytes;
}

static unsigned bochs_snapshot_size(void)
{
	return _hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8);
}

static void bochs_get_phys_window(unsigned *phys, unsigned *size)
{
	if (phys)
		*phys = _fb_phys;
	if (size)
		*size = bochs_snapshot_size();
}

static void bochs_snapshot_save(void *dst, unsigned size)
{
	unsigned need = bochs_snapshot_size();

	if (!dst || size < need)
		return;
	memcpy(dst, (const void *)(uintptr_t)_fb_phys, need);
}

static void bochs_snapshot_restore(const void *src, unsigned size)
{
	unsigned need = bochs_snapshot_size();

	if (!src || size < need)
		return;
	memcpy((void *)(uintptr_t)_fb_phys, src, need);
	memcpy((void *)_fb_buffer, src, need);
	dirty_begin = dirty_end = 0;
}

static unsigned short bga_read_register(unsigned short idx);

static void bochs_sync_mode(void)
{
	unsigned width;
	unsigned height;

	width = bga_read_register(VBE_DISPI_INDEX_XRES);
	height = bga_read_register(VBE_DISPI_INDEX_YRES);
	if (width && height &&
	    (width != _hw_resolution_x || height != _hw_resolution_y) &&
	    !bochs_resize_shadow(width, height))
		return;
	if (width > 0 && height > 0)
		bochs_ensure_fb_mapping(width, height);
	if (width > 0)
		_hw_resolution_x = width;
	if (height > 0)
		_hw_resolution_y = height;
}

/* ── BGA / VBE hardware ───────────────────────────────────────────────────── */

static void bga_write_register(unsigned short idx, unsigned short val)
{
	port_write_word(VBE_DISPI_IOPORT_INDEX, idx);
	port_write_word(VBE_DISPI_IOPORT_DATA, val);
}

static unsigned short bga_read_register(unsigned short idx)
{
	port_write_word(VBE_DISPI_IOPORT_INDEX, idx);
	return port_read_word(VBE_DISPI_IOPORT_DATA);
}

static int bga_is_available(void)
{
	if (bga_read_register(VBE_DISPI_INDEX_ID) != VBE_DISPI_ID5) {
		bga_write_register(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
		return bga_read_register(VBE_DISPI_INDEX_ID) == VBE_DISPI_ID5;
	}
	return 1;
}

static void bga_set_video_mode(unsigned int Width, unsigned int Height,
			       unsigned int BitDepth, int UseLinearFrameBuffer,
			       int ClearVideoMemory)
{
	bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
	bga_write_register(VBE_DISPI_INDEX_XRES, Width);
	bga_write_register(VBE_DISPI_INDEX_YRES, Height);
	bga_write_register(VBE_DISPI_INDEX_BPP, BitDepth);
	bga_write_register(
		VBE_DISPI_INDEX_ENABLE,
		VBE_DISPI_ENABLED |
			(UseLinearFrameBuffer ? VBE_DISPI_LFB_ENABLED : 0) |
			(ClearVideoMemory ? 0 : VBE_DISPI_NOCLEARMEM));
}

static int bochs_probe(unsigned device)
{
	if (!framebuffer_available() || !bga_is_available())
		return 0;
	if (!bochs_resize_shadow(VGA_RESOLUTION_X, VGA_RESOLUTION_Y))
		return 0;

	bga_set_video_mode(VGA_RESOLUTION_X, VGA_RESOLUTION_Y, VGA_COLOR_DEPTH,
			   1, 1);

	unsigned fb_phys = pci_read_field(device, PCI_BAR0, 4) & ~15U;

	if (!fb_phys)
		return 0;

	unsigned fb_size =
		VGA_RESOLUTION_X * VGA_RESOLUTION_Y * (VGA_COLOR_DEPTH / 8);
	unsigned a;
	for (a = fb_phys; a < fb_phys + fb_size; a += PAGE_SIZE)
		if (mm_map_io(a) == 1)
			arch_mm_invalidate(a);
	_fb_phys = fb_phys;
	_fb_mapped_bytes = fb_size;

	_hw_resolution_x = VGA_RESOLUTION_X;
	_hw_resolution_y = VGA_RESOLUTION_Y;

	return 1;
}

/* ── Driver descriptor ────────────────────────────────────────────────────── */

static void bochs_surface(framebuffer_surface *surface)
{
	surface->pixels = (uint32_t *)(uintptr_t)_fb_buffer;
	surface->width = _hw_resolution_x;
	surface->height = _hw_resolution_y;
	surface->pitch = _hw_resolution_x * 4;
}
static const framebuffer_ops bochs_drv = {
	.surface = bochs_surface,
	.sync_mode = bochs_sync_mode,
	.damage = bochs_dirty,
	.present = bochs_present,

	.get_phys_window = bochs_get_phys_window,
	.snapshot_size = bochs_snapshot_size,
	.snapshot_save = bochs_snapshot_save,
	.snapshot_restore = bochs_snapshot_restore,
};

static int bochs_probe_pci(uint32_t device, uint16_t vendor, uint16_t id,
			   const pci_device_id *match)
{
	if (!bochs_probe(device))
		return -ENODEV;
	framebuffer_register(&bochs_drv);
	return 0;
}

void bochs_console_init(unsigned device)
{
	if (pci_read_field(device, PCI_CLASS, 1) == 3 &&
	    pci_read_field(device, PCI_SUBCLASS, 1) == 0 && bochs_probe(device))
		framebuffer_register(&bochs_drv);
}

static const pci_device_id bochs_ids[] = {
	{ .vendor_id = 0x1234, .device_id = 0x1111 },
};

static driver_t bochs_driver = {
	.name = "bochs-vga",
	.bus = DEVICE_BUS_PCI,
	.pci_ids = bochs_ids,
	.pci_id_count = 1,
	.early = 1,
	.probe_pci = bochs_probe_pci,
};
DRIVER_REGISTER(bochs_driver);
