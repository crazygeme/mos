#include <int/int.h>
#include <boot/multiboot.h>
#include <device/pci.h>
#include <driver/driver.h>
#include <errno.h>
#include <device/vga.h>
#include <driver/video/bochs.h>

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
#include <device/font.h>
#include <mm/mm.h>
#include <macro.h>
#include <mm/mmu.h>
#include <lib/port.h>
#include <lib/klib.h>

static const unsigned char bit_mask[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };

static uintptr_t _fb_buffer;
static unsigned shadow_bytes, dirty_begin, dirty_end;
static unsigned _fb_phys;
static unsigned _fb_mapped_bytes;
static unsigned _hw_resolution_x;
static unsigned _hw_resolution_y;
static unsigned _window_char_width;
static unsigned _window_char_height;
static const fb_font_t *_font = NULL;

static void bochs_dirty(unsigned begin, unsigned end)
{
	if (!dirty_end || begin < dirty_begin)
		dirty_begin = begin;
	if (end > dirty_end)
		dirty_end = end;
}

static void bochs_flush_text(void)
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

/* ── Pixel rendering ──────────────────────────────────────────────────────── */

static void render_cell(const tty_cell_t *cell, int col, int row)
{
	if (!_font)
		return;

	unsigned *disp = (unsigned *)(uintptr_t)_fb_buffer;
	int px = col * (int)_font->width;
	int py = row * (int)_font->height;
	int i, j;
	bochs_dirty((py * _hw_resolution_x + px) * 4,
		    ((py + _font->height - 1) * _hw_resolution_x + px +
		     _font->width) *
			    4);

	for (i = 0; i < _font->height; i++) {
		unsigned char bits =
			_font->glyphs[(unsigned char)cell->ch * _font->height +
				      i];
		unsigned *rowp = disp + (py + i) * (int)_hw_resolution_x + px;
		for (j = 0; j < _font->width; j++)
			rowp[j] = (bits & bit_mask[j]) ? cell->fg : cell->bg;
	}
}

static void render_cursor_cell(int col, int row, char ch, unsigned fg,
			       unsigned bg, unsigned cursor_color)
{
	if (!_font)
		return;

	unsigned *disp = (unsigned *)(uintptr_t)_fb_buffer;
	int px = col * (int)_font->width;
	int py = row * (int)_font->height;
	int i, j;
	bochs_dirty((py * _hw_resolution_x + px) * 4,
		    ((py + _font->height - 1) * _hw_resolution_x + px +
		     _font->width) *
			    4);

	for (i = 0; i < _font->height; i++) {
		unsigned char bits_ch =
			_font->glyphs[(unsigned char)ch * _font->height + i];
		unsigned char bits_cur = _font->cursor_glyphs[i];
		unsigned *rowp = disp + (py + i) * (int)_hw_resolution_x + px;
		for (j = 0; j < _font->width; j++) {
			unsigned char m = bit_mask[j];
			if (bits_cur & m)
				rowp[j] = cursor_color;
			else if (bits_ch & m)
				rowp[j] = fg;
			else
				rowp[j] = bg;
		}
	}
}

/* ── Driver ops ───────────────────────────────────────────────────────────── */

static void bochs_get_char_dims(unsigned *cols, unsigned *rows)
{
	*cols = _window_char_width;
	*rows = _window_char_height;
}

static void bochs_putcell(const tty_cell_t *cell, int col, int row)
{
	render_cell(cell, col, row);
}

static void bochs_cursor_update(unsigned old_pos, unsigned new_pos,
				const tty_cell_t *cells, unsigned cols)
{
	render_cell(&cells[old_pos], (int)(old_pos % cols),
		    (int)(old_pos / cols));

	int new_col = (int)(new_pos % cols);
	int new_row = (int)(new_pos / cols);
	const tty_cell_t *nc = &cells[new_pos];

	if (nc->ch == '\0' || nc->ch == ' ')
		render_cursor_cell(new_col, new_row, ' ', VGA_COLOR_WHITE,
				   nc->bg, VGA_COLOR_WHITE);
	else
		render_cursor_cell(new_col, new_row, nc->ch, nc->fg, nc->bg,
				   VGA_COLOR_WHITE);
}

static void bochs_redraw(const tty_cell_t *cells, unsigned cols, unsigned rows,
			 unsigned cursor_pos)
{
	unsigned i;
	unsigned total = cols * rows;
	bochs_dirty(0, bochs_snapshot_size());

	memset((char *)(uintptr_t)_fb_buffer, 0,
	       _hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8));

	for (i = 0; i < total; i++)
		render_cell(&cells[i], (int)(i % cols), (int)(i / cols));

	{
		unsigned nc = cursor_pos % cols;
		unsigned nr = cursor_pos / cols;
		const tty_cell_t *c = &cells[cursor_pos];

		if (c->ch == '\0' || c->ch == ' ')
			render_cursor_cell((int)nc, (int)nr, ' ',
					   VGA_COLOR_WHITE, c->bg,
					   VGA_COLOR_WHITE);
		else
			render_cursor_cell((int)nc, (int)nr, c->ch, c->fg,
					   c->bg, VGA_COLOR_WHITE);
	}
	bochs_flush_text();
}

static void bochs_scroll_line_px(void)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	unsigned copy_size = bpr * (_window_char_height - 1);
	char *fb = (char *)(uintptr_t)_fb_buffer;

	memmove(fb, fb + bpr, copy_size);
	memset(fb + copy_size, 0, bpr);
	bochs_dirty(0, copy_size + bpr);
}

static void bochs_scroll_region_px(unsigned top_row, unsigned bot_row)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	char *fb = (char *)(uintptr_t)_fb_buffer;

	memmove(fb + top_row * bpr, fb + (top_row + 1) * bpr,
		(bot_row - top_row) * bpr);
	memset(fb + bot_row * bpr, 0, bpr);
	bochs_dirty(top_row * bpr, (bot_row + 1) * bpr);
}

static void bochs_insert_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	char *fb = (char *)(uintptr_t)_fb_buffer;

	bochs_dirty(row * bpr, (bot_row + 1) * bpr);
	if (n >= bot_row - row + 1) {
		memset(fb + row * bpr, 0, (bot_row - row + 1) * bpr);
		return;
	}
	unsigned move_rows = bot_row - row + 1 - n;
	memmove(fb + (row + n) * bpr, fb + row * bpr, move_rows * bpr);
	memset(fb + row * bpr, 0, n * bpr);
}

static void bochs_delete_lines_px(unsigned row, unsigned bot_row, unsigned n)
{
	if (!_font)
		return;

	unsigned bpr = _hw_resolution_x * (unsigned)_font->height *
		       (VGA_COLOR_DEPTH / 8);
	char *fb = (char *)(uintptr_t)_fb_buffer;

	bochs_dirty(row * bpr, (bot_row + 1) * bpr);
	if (n >= bot_row - row + 1) {
		memset(fb + row * bpr, 0, (bot_row - row + 1) * bpr);
		return;
	}
	unsigned move_rows = bot_row - row + 1 - n;
	memmove(fb + row * bpr, fb + (row + n) * bpr, move_rows * bpr);
	memset(fb + (bot_row - n + 1) * bpr, 0, n * bpr);
}

static void bochs_clear_screen(void)
{
	bochs_dirty(0, bochs_snapshot_size());
	memset((char *)(uintptr_t)_fb_buffer, 0,
	       _hw_resolution_x * _hw_resolution_y * (VGA_COLOR_DEPTH / 8));
}

static void bochs_cursor_erase(unsigned pos, const tty_cell_t *cells,
			       unsigned cols)
{
	render_cell(&cells[pos], (int)(pos % cols), (int)(pos / cols));
}

static void bochs_change_font(const char *name)
{
	const fb_font_t *f = font_get(name);
	if (f)
		_font = f;
}

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
	if (_font) {
		_window_char_width = _hw_resolution_x / (unsigned)_font->width;
		_window_char_height =
			_hw_resolution_y / (unsigned)_font->height;
	}
}

static int bochs_is_char_visible(unsigned char c)
{
	if (isprint(c))
		return 1;
	if (!_font)
		return 0;
	return (unsigned)c < (unsigned)_font->charcount;
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
	if (!fb_available() || !bga_is_available())
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

	bochs_change_font("vga16");

	_window_char_width = _hw_resolution_x / (unsigned)_font->width;
	_window_char_height = _hw_resolution_y / (unsigned)_font->height;

	return 1;
}

/* ── Driver descriptor ────────────────────────────────────────────────────── */

static const fb_drv_t bochs_drv = {
	.get_char_dims = bochs_get_char_dims,
	.putcell = bochs_putcell,
	.redraw = bochs_redraw,
	.cursor_update = bochs_cursor_update,
	.scroll_line_px = bochs_scroll_line_px,
	.scroll_region_px = bochs_scroll_region_px,
	.insert_lines_px = bochs_insert_lines_px,
	.delete_lines_px = bochs_delete_lines_px,
	.clear_screen = bochs_clear_screen,
	.change_font = bochs_change_font,
	.sync_mode = bochs_sync_mode,
	.flush_text = bochs_flush_text,
	.get_phys_window = bochs_get_phys_window,
	.snapshot_size = bochs_snapshot_size,
	.snapshot_save = bochs_snapshot_save,
	.snapshot_restore = bochs_snapshot_restore,
	.is_char_visible = bochs_is_char_visible,
	.cursor_erase = bochs_cursor_erase,
};

static int bochs_probe_pci(uint32_t device, uint16_t vendor, uint16_t id,
			   const pci_device_id *match)
{
	if (!bochs_probe(device))
		return -ENODEV;
	fb_activate(&bochs_drv);
	return 0;
}

void bochs_console_init(unsigned device)
{
	if (pci_read_field(device, PCI_CLASS, 1) == 3 &&
	    pci_read_field(device, PCI_SUBCLASS, 1) == 0 && bochs_probe(device))
		fb_activate(&bochs_drv);
}

static const pci_device_id bochs_ids[] = {
	{ .vendor_id = 0x1234, .device_id = 0x1111 },
};

static driver_t bochs_driver = {
	.name = "bochs-vga",
	.type = DRIVER_TYPE_VIDEO,
	.bus = DEVICE_BUS_PCI,
	.pci_ids = bochs_ids,
	.pci_id_count = 1,
	.early = 1,
	.probe_pci = bochs_probe_pci,
};
DRIVER_REGISTER(bochs_driver);
