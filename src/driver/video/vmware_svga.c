#include <int/int.h>
#include <boot/multiboot.h>
#include <driver/driver.h>
#include <device/framebuffer.h>
#include <errno.h>
#include <mm/mm.h>
#include <macro.h>
#include <mm/mmu.h>
#include <lib/port.h>
#include <lib/klib.h>

/* VMware SVGA2 PCI IDs */
#define VMSVGA_VENDOR_ID 0x15AD
#define VMSVGA_DEVICE_ID 0x0405

/* I/O port offsets from BAR0 */
#define SVGA_INDEX_PORT 0
#define SVGA_VALUE_PORT 1

/* Register indices */
#define SVGA_REG_ID 0
#define SVGA_REG_ENABLE 1
#define SVGA_REG_WIDTH 2
#define SVGA_REG_HEIGHT 3
#define SVGA_REG_BPP 7
#define SVGA_REG_CAPABILITIES 17
#define SVGA_REG_MEM_SIZE 19
#define SVGA_REG_CONFIG_DONE 20
#define SVGA_REG_SYNC 21
#define SVGA_REG_BUSY 22

/* SVGA2 device ID */
#define SVGA_ID_2 (0x900000UL << 8 | 2) /* 0x90000002 */

/* FIFO header dword indices */
#define SVGA_FIFO_MIN 0
#define SVGA_FIFO_MAX 1
#define SVGA_FIFO_NEXT_CMD 2
#define SVGA_FIFO_STOP 3

/* FIFO commands */
#define SVGA_CMD_UPDATE 1
#define SVGA_CMD_RECT_FILL 2
#define SVGA_CMD_RECT_COPY 3
#define SVGA_CAP_RECT_FILL 1
#define SVGA_CAP_RECT_COPY 2

static unsigned short _iobase;
static uint32_t *_fifo;
static uint32_t _caps;

static unsigned _fb_buffer;
static unsigned _fb_phys;
static unsigned _fb_mapped_bytes;
static unsigned _hw_resolution_x;
static unsigned _hw_resolution_y;

static void svga_update(unsigned x, unsigned y, unsigned w, unsigned h);

static unsigned vmsvga_map_mmio_window(unsigned phys, unsigned size)
{
	unsigned begin;
	unsigned end;
	unsigned a;

	if (size == 0)
		return 0;

	begin = phys & PAGE_SIZE_MASK;
	end = (phys + size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
	if (end <= begin)
		return 0;

	for (a = begin; a < end; a += PAGE_SIZE) {
		vaddr_t virt = a;
		if (mm_map_io(a) != 1)
			return 0;
		arch_mm_invalidate(virt & PAGE_SIZE_MASK);
	}

	return phys;
}

static void vmsvga_ensure_fb_mapping(unsigned width, unsigned height)
{
	unsigned need_bytes;
	unsigned a;

	if (width == 0 || height == 0)
		return;

	need_bytes = width * height * (VGA_COLOR_DEPTH / 8);
	if (need_bytes <= _fb_mapped_bytes)
		return;

	for (a = _fb_phys + _fb_mapped_bytes; a < _fb_phys + need_bytes;
	     a += PAGE_SIZE) {
		vaddr_t virt = a;
		if (mm_map_io(a) != 1)
			return;
		arch_mm_invalidate(virt & PAGE_SIZE_MASK);
	}
	_fb_mapped_bytes = need_bytes;
}

static unsigned vmsvga_snapshot_size(void)
{
	return _hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8);
}

static void vmsvga_get_phys_window(unsigned *phys, unsigned *size)
{
	if (phys)
		*phys = _fb_phys;
	if (size)
		*size = vmsvga_snapshot_size();
}

static void vmsvga_snapshot_save(void *dst, unsigned size)
{
	unsigned need = vmsvga_snapshot_size();

	if (!dst || size < need)
		return;
	memcpy(dst, (const void *)(uintptr_t)_fb_buffer, need);
}

static void vmsvga_snapshot_restore(const void *src, unsigned size)
{
	unsigned need = vmsvga_snapshot_size();

	if (!src || size < need)
		return;
	memcpy((void *)(uintptr_t)_fb_buffer, src, need);
	svga_update(0, 0, _hw_resolution_x, _hw_resolution_y);
}

/* ── Register I/O ─────────────────────────────────────────────────────────── */

static void svga_write_reg(unsigned index, unsigned value)
{
	port_write_dword(_iobase + SVGA_INDEX_PORT, index);
	port_write_dword(_iobase + SVGA_VALUE_PORT, value);
}

static unsigned svga_read_reg(unsigned index)
{
	port_write_dword(_iobase + SVGA_INDEX_PORT, index);
	return port_read_dword(_iobase + SVGA_VALUE_PORT);
}

/* ── FIFO ─────────────────────────────────────────────────────────────────── */

static void fifo_write(uint32_t val)
{
	uint32_t next = _fifo[SVGA_FIFO_NEXT_CMD];

	_fifo[next / 4] = val;
	next += 4;
	if (next >= _fifo[SVGA_FIFO_MAX])
		next = _fifo[SVGA_FIFO_MIN];

	while (next == _fifo[SVGA_FIFO_STOP]) {
		svga_write_reg(SVGA_REG_SYNC, 1);
		while (svga_read_reg(SVGA_REG_BUSY))
			;
	}

	_fifo[SVGA_FIFO_NEXT_CMD] = next;
}

static void fifo_sync(void)
{
	svga_write_reg(SVGA_REG_SYNC, 1);
	while (svga_read_reg(SVGA_REG_BUSY))
		;
}

/* Pixel updates submitted to the device FIFO. */

static void svga_update(unsigned x, unsigned y, unsigned w, unsigned h)
{
	fifo_write(SVGA_CMD_UPDATE);
	fifo_write(x);
	fifo_write(y);
	fifo_write(w);
	fifo_write(h);
	/* The refresh callback synchronizes user-space framebuffer updates. */
}

static int vmsvga_copy_area(unsigned sx, unsigned sy, unsigned dx, unsigned dy,
			    unsigned width, unsigned height)
{
	if (!(_caps & SVGA_CAP_RECT_COPY))
		return 0;
	fifo_write(SVGA_CMD_RECT_COPY);
	fifo_write(sx);
	fifo_write(sy);
	fifo_write(dx);
	fifo_write(dy);
	fifo_write(width);
	fifo_write(height);
	fifo_sync();
	return 1;
}
static int vmsvga_fill_rect(uint32_t color, unsigned x, unsigned y,
			    unsigned width, unsigned height)
{
	if (!(_caps & SVGA_CAP_RECT_FILL))
		return 0;
	fifo_write(SVGA_CMD_RECT_FILL);
	fifo_write(color);
	fifo_write(x);
	fifo_write(y);
	fifo_write(width);
	fifo_write(height);
	fifo_sync();
	return 1;
}

static void vmsvga_sync_mode(void)
{
	unsigned width;
	unsigned height;

	width = svga_read_reg(SVGA_REG_WIDTH);
	height = svga_read_reg(SVGA_REG_HEIGHT);
	if (width > 0 && height > 0)
		vmsvga_ensure_fb_mapping(width, height);
	if (width > 0)
		_hw_resolution_x = width;
	if (height > 0)
		_hw_resolution_y = height;
}

static void vmsvga_flush(void)
{
	svga_update(0, 0, _hw_resolution_x, _hw_resolution_y);
	/*
	 * The periodic graphics-refresh path relies on flush() to make user-space
	 * framebuffer writes visible even when no other accelerated blit happens.
	 * An UPDATE alone is only queued in the FIFO; force a SYNC here so QEMU
	 * processes that queued update immediately instead of appearing to "wake
	 * up" only on the next unrelated input event.
	 */
	fifo_sync();
}

/* ── PCI discovery ────────────────────────────────────────────────────────── */

typedef struct {
	uint32_t iobase;
	uint32_t fb_phys;
	uint32_t fifo_phys;
} vmsvga_pci_t;

static int vmsvga_probe(unsigned device)
{
	vmsvga_pci_t pci;
	if (!framebuffer_available())
		return 0;
	pci.iobase = pci_read_field(device, PCI_BAR0, 4) & ~3U;
	pci.fb_phys = pci_read_field(device, PCI_BAR1, 4) & ~15U;
	pci.fifo_phys = pci_read_field(device, PCI_BAR2, 4) & ~15U;

	if (!pci.iobase)
		return 0;

	_iobase = (unsigned short)pci.iobase;

	svga_write_reg(SVGA_REG_ID, SVGA_ID_2);
	if (svga_read_reg(SVGA_REG_ID) != SVGA_ID_2)
		return 0;

	uint32_t fifo_size = svga_read_reg(SVGA_REG_MEM_SIZE);
	uint32_t fifo_virt = vmsvga_map_mmio_window(pci.fifo_phys, fifo_size);
	if (fifo_virt == 0)
		return 0;
	_fifo = (uint32_t *)(uintptr_t)fifo_virt;

	_fifo[SVGA_FIFO_MIN] = 4 * sizeof(uint32_t);
	_fifo[SVGA_FIFO_MAX] = fifo_size;
	_fifo[SVGA_FIFO_NEXT_CMD] = _fifo[SVGA_FIFO_MIN];
	_fifo[SVGA_FIFO_STOP] = _fifo[SVGA_FIFO_MIN];

	svga_write_reg(SVGA_REG_WIDTH, VGA_RESOLUTION_X);
	svga_write_reg(SVGA_REG_HEIGHT, VGA_RESOLUTION_Y);
	svga_write_reg(SVGA_REG_BPP, VGA_COLOR_DEPTH);
	svga_write_reg(SVGA_REG_ENABLE, 1);
	svga_write_reg(SVGA_REG_CONFIG_DONE, 1);

	uint32_t fb_size =
		VGA_RESOLUTION_X * VGA_RESOLUTION_Y * (VGA_COLOR_DEPTH / 8);
	uint32_t fb_virt = vmsvga_map_mmio_window(pci.fb_phys, fb_size);
	if (fb_virt == 0)
		return 0;
	_fb_phys = pci.fb_phys;
	_fb_buffer = fb_virt;
	_fb_mapped_bytes = fb_size;

	memset((char *)(uintptr_t)_fb_buffer, 0, fb_size);

	_caps = svga_read_reg(SVGA_REG_CAPABILITIES);
	_hw_resolution_x = VGA_RESOLUTION_X;
	_hw_resolution_y = VGA_RESOLUTION_Y;

	return 1;
}

/* ── Driver descriptor ────────────────────────────────────────────────────── */

static unsigned dirty_begin, dirty_end;
static void vmsvga_damage(unsigned begin, unsigned end)
{
	if (!dirty_end || begin < dirty_begin)
		dirty_begin = begin;
	if (end > dirty_end)
		dirty_end = end;
}
static void vmsvga_present(void)
{
	if (!dirty_end)
		return;
	unsigned pitch = _hw_resolution_x * 4;
	unsigned top = dirty_begin / pitch;
	unsigned bottom = (dirty_end - 1) / pitch;
	svga_update(0, top, _hw_resolution_x, bottom - top + 1);
	dirty_begin = dirty_end = 0;
}
static void vmsvga_surface(framebuffer_surface *surface)
{
	surface->pixels = (uint32_t *)(uintptr_t)_fb_buffer;
	surface->width = _hw_resolution_x;
	surface->height = _hw_resolution_y;
	surface->pitch = _hw_resolution_x * 4;
}
static const framebuffer_ops vmsvga_drv = {
	.surface = vmsvga_surface,
	.sync_mode = vmsvga_sync_mode,
	.damage = vmsvga_damage,
	.present = vmsvga_present,
	.refresh = vmsvga_flush,
	.copy_area = vmsvga_copy_area,
	.fill_rect = vmsvga_fill_rect,
	.get_phys_window = vmsvga_get_phys_window,
	.snapshot_size = vmsvga_snapshot_size,
	.snapshot_save = vmsvga_snapshot_save,
	.snapshot_restore = vmsvga_snapshot_restore,
};

static int vmsvga_probe_pci(uint32_t device, uint16_t vendor, uint16_t id,
			    const pci_device_id *match)
{
	if (!vmsvga_probe(device))
		return -ENODEV;
	framebuffer_register(&vmsvga_drv);
	return 0;
}

static const pci_device_id vmsvga_ids[] = {
	{ .vendor_id = VMSVGA_VENDOR_ID, .device_id = VMSVGA_DEVICE_ID },
};
static driver_t vmsvga_driver = {
	.name = "vmware-svga",
	.bus = DEVICE_BUS_PCI,
	.pci_ids = vmsvga_ids,
	.pci_id_count = 1,
	.early = 1,
	.probe_pci = vmsvga_probe_pci,
};
DRIVER_REGISTER(vmsvga_driver);
